/*
 * cpubench - CPU throughput micro-benchmarks, auto-tuned for the host CPU.
 *
 * Every test runs once on a single thread and once on all CPUs of the
 * process affinity mask (one pinned thread per CPU):
 *
 *   int     scalar 64-bit integer ALU throughput (10 independent
 *           add/rotate/xor dependency chains)                   Gops/s
 *   fp64    peak double precision FMA throughput                GFLOPS
 *   fp32    peak single precision FMA throughput                GFLOPS
 *   sha256  portable scalar SHA-256 over an L1-resident buffer  MB/s
 *   sort    quicksort of 64Ki random 32-bit keys (L2 resident)  Melem/s
 *
 * The FMA kernels are compiled in several shapes (vector width x number of
 * independent accumulators). The fastest shape for this micro-architecture
 * is selected at run time for each mode, so the result approaches the
 * theoretical peak (vector width x FMA units x frequency) on any CPU.
 * Build with -O3 -march=native so the widest vector ISA is used.
 *
 * Author: Oleg Zhurko <oleg@zhurko.de>
 * SPDX-License-Identifier: MIT
 */
#include "benchutil.h"
#include "sha256.h"

#include <getopt.h>
#include <stdatomic.h>

#define MAX_VARIANTS 16
#define BATCH_SECONDS 0.002 /* one kernel call; bounds the stop latency */

typedef void (*kernel_fn)(void *state, uint64_t iters);

typedef struct {
    const char *name;     /* e.g. "512bit/12acc" */
    kernel_fn fn;
    double work_per_iter; /* work units done by one iteration */
} variant_t;

typedef struct {
    const char *name, *unit, *desc;
    double unit_scale; /* work units per reported unit */
    const variant_t *variants;
    int nvariants;
    size_t state_size;
    void (*state_init)(void *state, int tid);
} test_t;

/* ------------------------------------------------------------------ */
/* per-thread state                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    double fsink;
    uint64_t isink;
    uint64_t seed;
} st_basic_t;

#define SHA_BUF 4096
typedef struct {
    uint32_t h[8];
    uint8_t buf[SHA_BUF];
} st_sha_t;

#define SORT_N 65536
typedef struct {
    uint64_t rng;
    uint64_t isink;
    uint32_t a[SORT_N];
} st_sort_t;

static void init_basic(void *p, int tid)
{
    st_basic_t *st = p;
    st->seed = 0x9E3779B97F4A7C15ull * (uint64_t)(tid + 1);
}

static void init_sha(void *p, int tid)
{
    st_sha_t *st = p;
    uint64_t r = 0x1234567ull + (uint64_t)tid;
    for (int i = 0; i < SHA_BUF; i++)
        st->buf[i] = (uint8_t)bu_rand64(&r);
    for (int i = 0; i < 8; i++)
        st->h[i] = (uint32_t)bu_rand64(&r);
}

static void init_sort(void *p, int tid)
{
    st_sort_t *st = p;
    st->rng = 0xC0FFEE123ull + (uint64_t)tid * 7919;
}

/* ------------------------------------------------------------------ */
/* int: scalar ALU throughput                                          */
/* ------------------------------------------------------------------ */

#define INT_CHAINS 10
#define INT_OPS 3 /* add, rotate, xor per chain step */

/* Each op is a single 1-cycle instruction on x86-64 and AArch64 (in-place
 * add/rotate/xor with immediates), so Gops/s maps directly to ALU issue
 * rate rather than to the code generator's choice of register moves. */
static void k_int(void *p, uint64_t iters)
{
    st_basic_t *st = p;
    uint64_t a = st->seed, b = a + 1, c = a + 2, d = a + 3, e = a + 4;
    uint64_t f = a + 5, g = a + 6, h = a + 7, i = a + 8, j = a + 9;
    for (uint64_t n = 0; n < iters; n++) {
#define IS(x) x += 0x2B; x = (x << 13) | (x >> 51); x ^= 0x5A5A5A5Au;
        IS(a) IS(b) IS(c) IS(d) IS(e) IS(f) IS(g) IS(h) IS(i) IS(j)
#undef IS
        /* keep the chains in general purpose registers (no vectorization,
         * no folding across iterations) */
        __asm__("" : "+r"(a), "+r"(b), "+r"(c), "+r"(d), "+r"(e), "+r"(f), "+r"(g), "+r"(h),
                "+r"(i), "+r"(j));
    }
    st->isink += a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ i ^ j;
}

static const variant_t int_variants[] = {
    {"scalar/10chains", k_int, (double)INT_CHAINS * INT_OPS},
};

/* ------------------------------------------------------------------ */
/* fp32/fp64: FMA peak                                                 */
/* ------------------------------------------------------------------ */

/* Loaded once per kernel call so the compiler cannot constant-fold. The
 * recurrence x = x * m + a converges to 1.0, so values stay normal. */
static volatile double g_fma_mul = 0.99999;
static volatile double g_fma_add = 0.00001;

#define R8(M) M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7)
#define R12(M) R8(M) M(8) M(9) M(10) M(11)
#define R16(M) R12(M) M(12) M(13) M(14) M(15)
#define R24(M) R16(M) M(16) M(17) M(18) M(19) M(20) M(21) M(22) M(23)

#define FMA_DECL(i) v_t x##i = one + (elem_t)(i) * (elem_t)0.001;
#define FMA_STEP(i) x##i = x##i * m + a;
#define FMA_SUM(i) s += x##i;

/* clang splits 512-bit vector types into two 256-bit halves on CPUs where
 * it prefers 256-bit vectors (e.g. Intel server parts) unless told the
 * function really wants wide vectors. */
#if defined(__clang__) && defined(__AVX512F__)
#define WIDE_VEC __attribute__((min_vector_width(512)))
#else
#define WIDE_VEC
#endif

#define DEF_FMA(NAME, TYPE, W, REP)                                                    \
    static WIDE_VEC void NAME(void *p, uint64_t iters)                                 \
    {                                                                                  \
        typedef TYPE elem_t;                                                           \
        typedef elem_t v_t __attribute__((vector_size(W)));                            \
        st_basic_t *st = p;                                                            \
        const v_t zero = {0};                                                          \
        const v_t m = zero + (elem_t)g_fma_mul;                                        \
        const v_t a = zero + (elem_t)g_fma_add;                                        \
        const v_t one = zero + (elem_t)1;                                              \
        REP(FMA_DECL)                                                                  \
        for (uint64_t n = 0; n < iters; n++) {                                         \
            REP(FMA_STEP)                                                              \
        }                                                                              \
        v_t s = zero;                                                                  \
        REP(FMA_SUM)                                                                   \
        for (unsigned l = 0; l < (W) / sizeof(elem_t); l++)                            \
            st->fsink += (double)s[l];                                                 \
    }

#define FMAV(TN, TYPE, W, K) DEF_FMA(fma_##TN##_w##W##_k##K, TYPE, W, R##K)
#define FMAE(TN, TYPE, W, BITS, K) \
    {#BITS "bit/" #K "acc", fma_##TN##_w##W##_k##K, (double)(K) * ((W) / sizeof(TYPE)) * 2.0},

#if defined(__AVX512F__)
#define HAVE_W64 1
#else
#define HAVE_W64 0
#endif
#if defined(__AVX__)
#define HAVE_W32 1
#else
#define HAVE_W32 0
#endif
/* 32 architectural vector registers: room for 16-24 accumulators */
#if defined(__aarch64__) || defined(__AVX512VL__) || defined(__powerpc64__)
#define MANY_VREGS 1
#else
#define MANY_VREGS 0
#endif

#define DEF_FMA_SET(TN, TYPE)                                                          \
    FMA_W64(TN, TYPE)                                                                  \
    FMA_W32(TN, TYPE)                                                                  \
    FMAV(TN, TYPE, 16, 8)                                                              \
    FMAV(TN, TYPE, 16, 12)                                                             \
    FMA_W16_MANY(TN, TYPE)

#if HAVE_W64
#define FMA_W64(TN, TYPE) FMAV(TN, TYPE, 64, 8) FMAV(TN, TYPE, 64, 12) FMAV(TN, TYPE, 64, 16) FMAV(TN, TYPE, 64, 24)
#define FMA_W64_E(TN, TYPE) \
    FMAE(TN, TYPE, 64, 512, 8) FMAE(TN, TYPE, 64, 512, 12) FMAE(TN, TYPE, 64, 512, 16) FMAE(TN, TYPE, 64, 512, 24)
#else
#define FMA_W64(TN, TYPE)
#define FMA_W64_E(TN, TYPE)
#endif

#if HAVE_W32 && MANY_VREGS
#define FMA_W32(TN, TYPE) FMAV(TN, TYPE, 32, 8) FMAV(TN, TYPE, 32, 12) FMAV(TN, TYPE, 32, 16) FMAV(TN, TYPE, 32, 24)
#define FMA_W32_E(TN, TYPE) \
    FMAE(TN, TYPE, 32, 256, 8) FMAE(TN, TYPE, 32, 256, 12) FMAE(TN, TYPE, 32, 256, 16) FMAE(TN, TYPE, 32, 256, 24)
#elif HAVE_W32
#define FMA_W32(TN, TYPE) FMAV(TN, TYPE, 32, 8) FMAV(TN, TYPE, 32, 12)
#define FMA_W32_E(TN, TYPE) FMAE(TN, TYPE, 32, 256, 8) FMAE(TN, TYPE, 32, 256, 12)
#else
#define FMA_W32(TN, TYPE)
#define FMA_W32_E(TN, TYPE)
#endif

#if MANY_VREGS
#define FMA_W16_MANY(TN, TYPE) FMAV(TN, TYPE, 16, 16) FMAV(TN, TYPE, 16, 24)
#define FMA_W16_MANY_E(TN, TYPE) FMAE(TN, TYPE, 16, 128, 16) FMAE(TN, TYPE, 16, 128, 24)
#else
#define FMA_W16_MANY(TN, TYPE)
#define FMA_W16_MANY_E(TN, TYPE)
#endif

#define FMA_TABLE(TN, TYPE)                                                            \
    FMA_W64_E(TN, TYPE)                                                                \
    FMA_W32_E(TN, TYPE)                                                                \
    FMAE(TN, TYPE, 16, 128, 8)                                                         \
    FMAE(TN, TYPE, 16, 128, 12)                                                        \
    FMA_W16_MANY_E(TN, TYPE)

DEF_FMA_SET(f64, double)
DEF_FMA_SET(f32, float)

static const variant_t fp64_variants[] = {FMA_TABLE(f64, double)};
static const variant_t fp32_variants[] = {FMA_TABLE(f32, float)};

/* ------------------------------------------------------------------ */
/* sha256                                                              */
/* ------------------------------------------------------------------ */

static void k_sha(void *p, uint64_t iters)
{
    st_sha_t *st = p;
    for (uint64_t n = 0; n < iters; n++)
        sha256_blocks(st->h, st->buf, SHA_BUF / 64);
}

static const variant_t sha_variants[] = {
    {"scalar", k_sha, (double)SHA_BUF},
};

/* ------------------------------------------------------------------ */
/* sort                                                                */
/* ------------------------------------------------------------------ */

static void isort_u32(uint32_t *a, long n)
{
    for (long i = 1; i < n; i++) {
        uint32_t v = a[i];
        long j = i;
        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
}

/* Introsort-style quicksort: median of three, Hoare partition, recursion
 * on the smaller side, insertion sort for short ranges. */
static void sort_u32(uint32_t *a, long n)
{
    while (n > 24) {
        long mid = n / 2;
        uint32_t t;
        if (a[mid] < a[0]) { t = a[mid]; a[mid] = a[0]; a[0] = t; }
        if (a[n - 1] < a[0]) { t = a[n - 1]; a[n - 1] = a[0]; a[0] = t; }
        if (a[n - 1] < a[mid]) { t = a[n - 1]; a[n - 1] = a[mid]; a[mid] = t; }
        uint32_t pivot = a[mid];
        long i = -1, j = n;
        for (;;) {
            do i++; while (a[i] < pivot);
            do j--; while (a[j] > pivot);
            if (i >= j)
                break;
            t = a[i]; a[i] = a[j]; a[j] = t;
        }
        long left = j + 1; /* [0, j] and [j + 1, n) */
        if (left < n - left) {
            sort_u32(a, left);
            a += left;
            n -= left;
        } else {
            sort_u32(a + left, n - left);
            n = left;
        }
    }
    isort_u32(a, n);
}

static void k_sort(void *p, uint64_t iters)
{
    st_sort_t *st = p;
    for (uint64_t n = 0; n < iters; n++) {
        uint64_t r = st->rng;
        for (int i = 0; i < SORT_N; i++)
            st->a[i] = (uint32_t)(bu_rand64(&r) >> 32);
        st->rng = r;
        sort_u32(st->a, SORT_N);
        st->isink += st->a[SORT_N / 2];
    }
}

static const variant_t sort_variants[] = {
    {"quicksort", k_sort, (double)SORT_N},
};

/* ------------------------------------------------------------------ */
/* test table                                                          */
/* ------------------------------------------------------------------ */

#define NV(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const test_t g_tests[] = {
    {"int", "Gops/s", "scalar 64-bit integer ALU ops (add/rotate/xor, 10 chains)", 1e9,
     int_variants, NV(int_variants), sizeof(st_basic_t), init_basic},
    {"fp64", "GFLOPS", "double precision vector FMA peak", 1e9,
     fp64_variants, NV(fp64_variants), sizeof(st_basic_t), init_basic},
    {"fp32", "GFLOPS", "single precision vector FMA peak", 1e9,
     fp32_variants, NV(fp32_variants), sizeof(st_basic_t), init_basic},
    {"sha256", "MB/s", "portable scalar SHA-256 over a 4 KiB buffer", 1e6,
     sha_variants, NV(sha_variants), sizeof(st_sha_t), init_sha},
    {"sort", "Melem/s", "quicksort of 64Ki random uint32 keys", 1e6,
     sort_variants, NV(sort_variants), sizeof(st_sort_t), init_sort},
};
#define NTESTS NV(g_tests)

/* ------------------------------------------------------------------ */
/* harness                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    pthread_t th;
    int cpu, tid;
    const test_t *t;
    const variant_t *v;
    uint64_t batch, iters;
} worker_t;

static bu_barrier_t g_start;
static atomic_int g_stop;

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    if (w->cpu >= 0)
        bu_pin_self(w->cpu);
    void *st = bu_aligned_alloc(w->t->state_size); /* first touch after pinning */
    if (w->t->state_init)
        w->t->state_init(st, w->tid);
    bu_barrier_wait(&g_start);
    uint64_t n = 0;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        w->v->fn(st, w->batch);
        n += w->batch;
    }
    w->iters = n;
    free(st);
    return NULL;
}

/* Runs a kernel variant on `nthreads` threads (pinned round-robin to cpus[],
 * unpinned if cpus == NULL) for `sec` seconds; returns work units/second. */
static double run_threads(const test_t *t, const variant_t *v, int nthreads, const int *cpus,
                          int ncpus, uint64_t batch, double sec)
{
    worker_t *ws = bu_xcalloc((size_t)nthreads, sizeof *ws);
    bu_barrier_init(&g_start, (unsigned)nthreads + 1);
    atomic_store(&g_stop, 0);
    for (int i = 0; i < nthreads; i++) {
        ws[i].cpu = cpus ? cpus[i % ncpus] : -1;
        ws[i].tid = i;
        ws[i].t = t;
        ws[i].v = v;
        ws[i].batch = batch;
        if (pthread_create(&ws[i].th, NULL, worker_main, &ws[i]) != 0)
            bu_die("pthread_create failed");
    }
    bu_barrier_wait(&g_start);
    double t0 = bu_now();
    bu_sleep(sec);
    atomic_store(&g_stop, 1);
    double total = 0;
    for (int i = 0; i < nthreads; i++) {
        pthread_join(ws[i].th, NULL);
        total += (double)ws[i].iters;
    }
    double t1 = bu_now();
    bu_barrier_destroy(&g_start);
    free(ws);
    return total * v->work_per_iter / (t1 - t0);
}

/* Iterations per kernel call so that one call takes about BATCH_SECONDS. */
static uint64_t calibrate_batch(const test_t *t, const variant_t *v)
{
    void *st = bu_aligned_alloc(t->state_size);
    if (t->state_init)
        t->state_init(st, 0);
    v->fn(st, 1); /* warm up caches / page faults */
    uint64_t n = 1, batch = 1;
    for (;;) {
        double t0 = bu_now();
        v->fn(st, n);
        double dt = bu_now() - t0;
        if (dt >= BATCH_SECONDS / 4 || n >= (1ull << 40)) {
            double b = dt > 0 ? (double)n * BATCH_SECONDS / dt : (double)n;
            batch = b < 1 ? 1 : (uint64_t)b;
            break;
        }
        n *= 4;
    }
    free(st);
    return batch;
}

/* Picks the fastest kernel variant for the given thread configuration. */
static int pick_variant(const test_t *t, const uint64_t *batches, int nthreads, const int *cpus,
                        int ncpus, double sec, int verbose)
{
    if (t->nvariants == 1)
        return 0;
    int best = 0;
    double bestv = -1;
    for (int i = 0; i < t->nvariants; i++) {
        double r = run_threads(t, &t->variants[i], nthreads, cpus, ncpus, batches[i], sec);
        if (verbose)
            fprintf(stderr, "    [%s, %d thread%s] %-14s %12.2f %s\n", t->name, nthreads,
                    nthreads == 1 ? "" : "s", t->variants[i].name, r / t->unit_scale, t->unit);
        if (r > bestv) {
            bestv = r;
            best = i;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* self test                                                           */
/* ------------------------------------------------------------------ */

static int selftest(void)
{
    if (sha256_selftest() != 0) {
        fprintf(stderr, "cpubench: SHA-256 self test failed\n");
        return -1;
    }
    st_sort_t *st = bu_aligned_alloc(sizeof *st);
    init_sort(st, 3);
    k_sort(st, 1);
    for (int i = 1; i < SORT_N; i++)
        if (st->a[i - 1] > st->a[i]) {
            fprintf(stderr, "cpubench: sort self test failed\n");
            free(st);
            return -1;
        }
    free(st);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    printf("Usage: cpubench [options]\n"
           "  -t, --tests LIST     tests to run (default: all): ");
    for (int i = 0; i < NTESTS; i++)
        printf("%s%s", g_tests[i].name, i + 1 < NTESTS ? "," : "\n");
    printf("  -T, --threads N      threads for the multi-thread run (default: all CPUs\n"
           "                       in the affinity mask, one pinned thread per CPU)\n"
           "  -d, --duration SEC   measurement time per test and mode (default 5)\n"
           "  -m, --mode MODE      1t, mt or both (default both)\n"
           "      --calib SEC      time per kernel variant during auto-selection (default 0.1)\n"
           "      --kv             also print machine readable RESULT lines\n"
           "  -v, --verbose        show every kernel variant tried\n"
           "  -l, --list           list tests\n"
           "  -h, --help\n");
}

int main(int argc, char **argv)
{
    const char *tests = NULL;
    double duration = 5, calib = 0.1;
    int threads = 0, kv = 0, verbose = 0, do1t = 1, domt = 1;

    static const struct option opts[] = {
        {"tests", required_argument, 0, 't'}, {"threads", required_argument, 0, 'T'},
        {"duration", required_argument, 0, 'd'}, {"mode", required_argument, 0, 'm'},
        {"calib", required_argument, 0, 'C'}, {"kv", no_argument, 0, 'K'},
        {"verbose", no_argument, 0, 'v'}, {"list", no_argument, 0, 'l'},
        {"help", no_argument, 0, 'h'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "t:T:d:m:vlh", opts, NULL)) != -1) {
        switch (c) {
        case 't': tests = optarg; break;
        case 'T': threads = atoi(optarg); break;
        case 'd': duration = atof(optarg); break;
        case 'C': calib = atof(optarg); break;
        case 'K': kv = 1; break;
        case 'v': verbose = 1; break;
        case 'm':
            do1t = strcmp(optarg, "mt") != 0;
            domt = strcmp(optarg, "1t") != 0;
            if (strcmp(optarg, "1t") && strcmp(optarg, "mt") && strcmp(optarg, "both"))
                bu_die("cpubench: --mode must be 1t, mt or both");
            break;
        case 'l':
            for (int i = 0; i < NTESTS; i++)
                printf("%-8s %-8s %s\n", g_tests[i].name, g_tests[i].unit, g_tests[i].desc);
            return 0;
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }
    if (duration <= 0 || calib <= 0)
        bu_die("cpubench: duration and calib must be positive");
    if (selftest() != 0)
        return 1;

    static int cpus[BU_MAX_CPUS];
    int ncpus = bu_affinity_cpus(cpus, BU_MAX_CPUS);
    bu_cpu_order(cpus, ncpus);
    if (threads <= 0)
        threads = ncpus;
    int cores = bu_count_cores(cpus, ncpus);
    int st_cpu = bu_fastest_cpu(cpus, ncpus); /* the 1-thread run is pinned here */

    printf("cpubench: %d CPUs in affinity mask (%d physical cores), 1-thread run on CPU %d, "
           "multi-thread run uses %d threads\n", ncpus, cores, st_cpu, threads);
    printf("cpubench: SIMD %s, %s\n", bu_simd_name(), bu_compiler());
    printf("\n%-8s %-8s %12s %12s %8s  %s\n", "test", "unit", "1 thread", "multi", "scaling",
           "kernel (1t / mt)");
    fflush(stdout);

    int ran = 0;
    for (int ti = 0; ti < NTESTS; ti++) {
        const test_t *t = &g_tests[ti];
        if (tests) { /* comma separated selection */
            int found = 0;
            const char *s = tests;
            size_t len = strlen(t->name);
            while (*s) {
                const char *e = strchr(s, ',');
                size_t n = e ? (size_t)(e - s) : strlen(s);
                if (n == len && strncmp(s, t->name, len) == 0)
                    found = 1;
                s += n + (e ? 1 : 0);
            }
            if (!found)
                continue;
        }
        ran++;
        uint64_t batches[MAX_VARIANTS];
        for (int v = 0; v < t->nvariants; v++)
            batches[v] = calibrate_batch(t, &t->variants[v]);

        double r1 = 0, rm = 0;
        int v1 = -1, vm = -1;
        if (do1t) {
            v1 = pick_variant(t, batches, 1, &st_cpu, 1, calib, verbose);
            r1 = run_threads(t, &t->variants[v1], 1, &st_cpu, 1, batches[v1], duration) /
                 t->unit_scale;
        }
        if (domt) {
            vm = pick_variant(t, batches, threads, cpus, ncpus, calib, verbose);
            rm = run_threads(t, &t->variants[vm], threads, cpus, ncpus, batches[vm], duration) /
                 t->unit_scale;
        }

        char s1[32] = "-", sm[32] = "-", sc[32] = "-", kern[96];
        if (do1t)
            snprintf(s1, sizeof s1, "%.2f", r1);
        if (domt)
            snprintf(sm, sizeof sm, "%.2f", rm);
        if (do1t && domt && r1 > 0)
            snprintf(sc, sizeof sc, "%.2fx", rm / r1);
        snprintf(kern, sizeof kern, "%s / %s", v1 >= 0 ? t->variants[v1].name : "-",
                 vm >= 0 ? t->variants[vm].name : "-");
        printf("%-8s %-8s %12s %12s %8s  %s\n", t->name, t->unit, s1, sm, sc, kern);
        if (kv && do1t)
            printf("RESULT test=%s mode=1t threads=1 value=%.4f unit=%s kernel=%s\n", t->name, r1,
                   t->unit, t->variants[v1].name);
        if (kv && domt)
            printf("RESULT test=%s mode=mt threads=%d value=%.4f unit=%s kernel=%s\n", t->name,
                   threads, rm, t->unit, t->variants[vm].name);
        fflush(stdout);
    }
    if (!ran)
        bu_die("cpubench: no test matches '%s' (see --list)", tests ? tests : "");
    return 0;
}
