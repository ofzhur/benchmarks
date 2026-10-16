/*
 * membench - memory bandwidth and latency benchmark, auto-sized for the host.
 *
 * Bandwidth kernels (STREAM byte accounting, decimal GB/s):
 *   read   s += a[i]               8 bytes per element
 *   write  a[i] = x                8 bytes per element
 *   copy   a[i] = b[i]            16 bytes per element
 *   triad  a[i] = b[i] + s * c[i] 24 bytes per element
 *
 * To find the physical maximum every kernel is measured with each available
 * implementation (explicit SIMD loop, non-temporal stores on x86, libc
 * memset/memcpy) and the best one is reported. Each array is sized to at
 * least 4x the total last-level cache, pages are first-touched by the
 * thread that later uses them (NUMA local), threads are spread across NUMA
 * nodes and physical cores, and the thread count is swept because the
 * bandwidth peak is often reached before all cores are busy.
 *
 * Latency: dependent loads through a random cyclic permutation of 64-byte
 * lines (defeats prefetchers) for working sets from 4 KiB to well beyond
 * the LLC, with transparent huge pages to keep TLB misses out of the DRAM
 * figure where the kernel allows it.
 *
 * Author: Oleg Zhurko <oleg@zhurko.de>
 * SPDX-License-Identifier: MIT
 */
#include "benchutil.h"

#include <float.h>
#include <getopt.h>
#include <stdatomic.h>
#include <sys/mman.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#if defined(__AVX512F__)
#define VBYTES 64
#elif defined(__AVX__)
#define VBYTES 32
#else
#define VBYTES 16
#endif
#define VLEN (VBYTES / 8)
typedef double vd __attribute__((vector_size(VBYTES)));

#if (defined(__x86_64__) || defined(__i386__)) && defined(__SSE2__)
#define HAVE_NT 1
#if VBYTES == 64
#define NT_STORE(p, v) _mm512_stream_pd((p), (__m512d)(v))
#elif VBYTES == 32
#define NT_STORE(p, v) _mm256_stream_pd((p), (__m256d)(v))
#else
#define NT_STORE(p, v) _mm_stream_pd((p), (__m128d)(v))
#endif
#define NT_FENCE() _mm_sfence()
#else
#define HAVE_NT 0
#endif

#if defined(__clang__) && defined(__AVX512F__)
#define WIDE_VEC __attribute__((min_vector_width(512)))
#else
#define WIDE_VEC
#endif

/* keep explicit loops as loops (GCC would turn some into memset/memcpy) */
#if defined(__GNUC__) && !defined(__clang__)
#define NO_IDIOM __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define NO_IDIOM
#endif

#if defined(__x86_64__) || defined(__i386__)
#define CPU_RELAX() _mm_pause()
#elif defined(__aarch64__)
#define CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")
#else
#define CPU_RELAX() do { } while (0)
#endif

#define HUGE_ALIGN (2u << 20)
#define CHUNK_ALIGN 512 /* elements (4 KiB) per-thread chunk alignment */
#define MAX_POINTS 64

/* ------------------------------------------------------------------ */
/* allocation                                                          */
/* ------------------------------------------------------------------ */

static size_t round_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

/* 2 MiB aligned anonymous mapping with transparent huge pages requested. */
static void *alloc_huge(size_t bytes)
{
    size_t len = round_up(bytes, HUGE_ALIGN);
    size_t map = len + HUGE_ALIGN;
    char *p = mmap(NULL, map, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    uintptr_t a = ((uintptr_t)p + HUGE_ALIGN - 1) & ~(uintptr_t)(HUGE_ALIGN - 1);
    size_t head = a - (uintptr_t)p, tail = map - head - len;
    if (head)
        munmap(p, head);
    if (tail)
        munmap((char *)a + len, tail);
#ifdef MADV_HUGEPAGE
    madvise((void *)a, len, MADV_HUGEPAGE);
#endif
    return (void *)a;
}

static void free_huge(void *p, size_t bytes)
{
    if (p)
        munmap(p, round_up(bytes, HUGE_ALIGN));
}

/* ------------------------------------------------------------------ */
/* spin barrier (workers only, one per CPU, low wake-up latency)       */
/* ------------------------------------------------------------------ */

typedef struct {
    atomic_uint count;
    atomic_uint gen;
    unsigned n;
} spin_barrier_t;

static void spin_wait(spin_barrier_t *b)
{
    unsigned gen = atomic_load_explicit(&b->gen, memory_order_acquire);
    if (atomic_fetch_add_explicit(&b->count, 1, memory_order_acq_rel) == b->n - 1) {
        atomic_store_explicit(&b->count, 0, memory_order_relaxed);
        atomic_fetch_add_explicit(&b->gen, 1, memory_order_release);
    } else {
        while (atomic_load_explicit(&b->gen, memory_order_acquire) == gen)
            CPU_RELAX();
    }
}

/* ------------------------------------------------------------------ */
/* bandwidth kernels                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    pthread_t th;
    int tid, cpu;
    size_t lo, hi; /* element range */
    double sink;
} bw_thread_t;

static double *gA, *gB, *gC;
static volatile double gScalar = 3.0;

static WIDE_VEC void k_read_vec(bw_thread_t *t)
{
    const double *a = gA + t->lo;
    size_t n = t->hi - t->lo, i = 0;
    vd s0 = {0}, s1 = {0}, s2 = {0}, s3 = {0};
    for (; i + 4 * VLEN <= n; i += 4 * VLEN) {
        s0 += *(const vd *)(a + i);
        s1 += *(const vd *)(a + i + VLEN);
        s2 += *(const vd *)(a + i + 2 * VLEN);
        s3 += *(const vd *)(a + i + 3 * VLEN);
    }
    double s = 0;
    for (; i < n; i++)
        s += a[i];
    vd v = s0 + s1 + s2 + s3;
    for (int l = 0; l < VLEN; l++)
        s += v[l];
    t->sink += s;
}

static WIDE_VEC NO_IDIOM void k_write_vec(bw_thread_t *t)
{
    double *a = gA + t->lo;
    size_t n = t->hi - t->lo, i = 0;
    const vd v = (vd){0} + 1.5; /* not a repeated byte: no memset idiom */
    for (; i + VLEN <= n; i += VLEN)
        *(vd *)(a + i) = v;
    for (; i < n; i++)
        a[i] = 1.5;
}

static void k_write_memset(bw_thread_t *t)
{
    memset(gA + t->lo, 0, (t->hi - t->lo) * sizeof(double));
}

static WIDE_VEC NO_IDIOM void k_copy_vec(bw_thread_t *t)
{
    double *a = gA + t->lo;
    const double *b = gB + t->lo;
    size_t n = t->hi - t->lo, i = 0;
    for (; i + VLEN <= n; i += VLEN)
        *(vd *)(a + i) = *(const vd *)(b + i);
    for (; i < n; i++)
        a[i] = b[i];
}

static void k_copy_memcpy(bw_thread_t *t)
{
    memcpy(gA + t->lo, gB + t->lo, (t->hi - t->lo) * sizeof(double));
}

static WIDE_VEC void k_triad_vec(bw_thread_t *t)
{
    double *a = gA + t->lo;
    const double *b = gB + t->lo, *c = gC + t->lo;
    const double s1 = gScalar;
    const vd s = (vd){0} + s1;
    size_t n = t->hi - t->lo, i = 0;
    for (; i + VLEN <= n; i += VLEN)
        *(vd *)(a + i) = *(const vd *)(b + i) + s * *(const vd *)(c + i);
    for (; i < n; i++)
        a[i] = b[i] + s1 * c[i];
}

#if HAVE_NT
static WIDE_VEC void k_write_nt(bw_thread_t *t)
{
    double *a = gA + t->lo;
    size_t n = t->hi - t->lo, i = 0;
    const vd v = (vd){0} + 1.5;
    for (; i + VLEN <= n; i += VLEN)
        NT_STORE(a + i, v);
    for (; i < n; i++)
        a[i] = 1.5;
    NT_FENCE();
}

static WIDE_VEC void k_copy_nt(bw_thread_t *t)
{
    double *a = gA + t->lo;
    const double *b = gB + t->lo;
    size_t n = t->hi - t->lo, i = 0;
    for (; i + VLEN <= n; i += VLEN)
        NT_STORE(a + i, *(const vd *)(b + i));
    for (; i < n; i++)
        a[i] = b[i];
    NT_FENCE();
}

static WIDE_VEC void k_triad_nt(bw_thread_t *t)
{
    double *a = gA + t->lo;
    const double *b = gB + t->lo, *c = gC + t->lo;
    const double s1 = gScalar;
    const vd s = (vd){0} + s1;
    size_t n = t->hi - t->lo, i = 0;
    for (; i + VLEN <= n; i += VLEN)
        NT_STORE(a + i, *(const vd *)(b + i) + s * *(const vd *)(c + i));
    for (; i < n; i++)
        a[i] = b[i] + s1 * c[i];
    NT_FENCE();
}
#endif

typedef void (*bw_fn)(bw_thread_t *);

typedef struct {
    int kernel; /* index into g_kernels */
    const char *method;
    bw_fn fn;
} bw_method_t;

static const struct {
    const char *name;
    int bytes; /* bytes moved per element */
} g_kernels[] = {{"read", 8}, {"write", 8}, {"copy", 16}, {"triad", 24}};
#define NKERNELS 4

static const bw_method_t g_methods[] = {
    {0, "simd", k_read_vec},
    {1, "simd", k_write_vec},
#if HAVE_NT
    {1, "nt-store", k_write_nt},
#endif
    {1, "memset", k_write_memset},
    {2, "simd", k_copy_vec},
#if HAVE_NT
    {2, "nt-store", k_copy_nt},
#endif
    {2, "memcpy", k_copy_memcpy},
    {3, "simd", k_triad_vec},
#if HAVE_NT
    {3, "nt-store", k_triad_nt},
#endif
};
#define NMETHODS ((int)(sizeof g_methods / sizeof g_methods[0]))

/* ------------------------------------------------------------------ */
/* bandwidth harness                                                   */
/* ------------------------------------------------------------------ */

static spin_barrier_t g_bar;
static int g_kernel_on[NKERNELS];
static int g_repeat = 5;
static double g_best_time[NMETHODS];

static void *bw_worker(void *arg)
{
    bw_thread_t *t = arg;
    bu_pin_self(t->cpu);
    for (size_t i = t->lo; i < t->hi; i++) { /* first touch: NUMA local pages */
        gA[i] = 1.0;
        gB[i] = 2.0;
        gC[i] = 0.5;
    }
    spin_wait(&g_bar);
    for (int m = 0; m < NMETHODS; m++) {
        if (!g_kernel_on[g_methods[m].kernel])
            continue;
        double best = DBL_MAX;
        for (int r = 0; r < g_repeat; r++) {
            spin_wait(&g_bar);
            double t0 = bu_now();
            g_methods[m].fn(t);
            spin_wait(&g_bar);
            double dt = bu_now() - t0;
            if (dt < best)
                best = dt;
            /* very large arrays: a few repetitions are enough. Every thread
             * sees the same decision because thread 0's timing is shared
             * through g_best_time before the next barrier. */
            if (t->tid == 0)
                g_best_time[m] = best;
            spin_wait(&g_bar);
            if (r >= 2 && g_best_time[m] > 0.5)
                break;
        }
    }
    return NULL;
}

/* Measures all selected methods with `nthreads` threads pinned to cpus[0..]. */
static void bw_point(int nthreads, const int *cpus, size_t nelem, double *gbs, int *method)
{
    size_t bytes = nelem * sizeof(double);
    gA = alloc_huge(bytes);
    gB = alloc_huge(bytes);
    gC = alloc_huge(bytes);
    if (!gA || !gB || !gC)
        bu_die("membench: cannot allocate 3 x %zu bytes", bytes);

    bw_thread_t *th = bu_xcalloc((size_t)nthreads, sizeof *th);
    size_t per = round_up((nelem + (size_t)nthreads - 1) / (size_t)nthreads, CHUNK_ALIGN);
    atomic_store(&g_bar.count, 0);
    atomic_store(&g_bar.gen, 0);
    g_bar.n = (unsigned)nthreads;
    for (int m = 0; m < NMETHODS; m++)
        g_best_time[m] = 0;
    for (int i = 0; i < nthreads; i++) {
        th[i].tid = i;
        th[i].cpu = cpus[i];
        th[i].lo = (size_t)i * per < nelem ? (size_t)i * per : nelem;
        th[i].hi = (size_t)(i + 1) * per < nelem ? (size_t)(i + 1) * per : nelem;
        if (pthread_create(&th[i].th, NULL, bw_worker, &th[i]) != 0)
            bu_die("membench: pthread_create failed");
    }
    for (int i = 0; i < nthreads; i++)
        pthread_join(th[i].th, NULL);
    free(th);

    for (int k = 0; k < NKERNELS; k++) {
        gbs[k] = 0;
        method[k] = -1;
    }
    for (int m = 0; m < NMETHODS; m++) {
        int k = g_methods[m].kernel;
        if (!g_kernel_on[k] || g_best_time[m] <= 0)
            continue;
        double v = (double)g_kernels[k].bytes * (double)nelem / g_best_time[m] / 1e9;
        if (v > gbs[k]) {
            gbs[k] = v;
            method[k] = m;
        }
    }
    free_huge(gA, bytes);
    free_huge(gB, bytes);
    free_huge(gC, bytes);
    gA = gB = gC = NULL;
}

/* ------------------------------------------------------------------ */
/* latency                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t bytes;
    double target; /* seconds of timed chasing */
    double ns;
    int cpu;
} lat_job_t;

static void *lat_worker(void *arg)
{
    lat_job_t *j = arg;
    bu_pin_self(j->cpu);
    const size_t line = 64;
    size_t n = j->bytes / line;
    if (n < 2)
        n = 2;
    char *buf = alloc_huge(n * line);
    uint32_t *perm = malloc(n * sizeof *perm);
    if (!buf || !perm)
        bu_die("membench: cannot allocate %zu bytes for the latency test", n * line);
    for (size_t i = 0; i < n; i++)
        perm[i] = (uint32_t)i;
    uint64_t rng = 0x5EED1234ABCDull ^ n;
    for (size_t i = n - 1; i > 0; i--) { /* Sattolo: one cycle through all lines */
        size_t k = (size_t)(((bu_rand64(&rng) >> 32) * (uint64_t)i) >> 32);
        uint32_t tmp = perm[i];
        perm[i] = perm[k];
        perm[k] = tmp;
    }
    for (size_t i = 0; i < n; i++)
        *(void **)(buf + i * line) = buf + (size_t)perm[i] * line;
    free(perm);

    void **p = (void **)buf;
    size_t warm = n < (2u << 20) ? n : (2u << 20);
    for (size_t i = 0; i < warm; i++)
        p = (void **)*p;

#define CHASE16                                                                  \
    p = (void **)*p; p = (void **)*p; p = (void **)*p; p = (void **)*p;          \
    p = (void **)*p; p = (void **)*p; p = (void **)*p; p = (void **)*p;          \
    p = (void **)*p; p = (void **)*p; p = (void **)*p; p = (void **)*p;          \
    p = (void **)*p; p = (void **)*p; p = (void **)*p; p = (void **)*p;

    uint64_t steps = 1u << 16;
    double t0 = bu_now();
    for (uint64_t s = 0; s < steps; s += 16) {
        CHASE16
    }
    double est = (bu_now() - t0) / (double)steps;
    steps = (uint64_t)(j->target / (est > 0 ? est : 1e-9));
    if (steps < (1u << 20))
        steps = 1u << 20;
    steps = (steps + 15) & ~(uint64_t)15;
    t0 = bu_now();
    for (uint64_t s = 0; s < steps; s += 16) {
        CHASE16
    }
    double dt = bu_now() - t0;
#undef CHASE16
    j->ns = dt * 1e9 / (double)steps;
    if ((uintptr_t)p == 1) /* keep the chain alive */
        fprintf(stderr, "?");
    free_huge(buf, n * line);
    return NULL;
}

static double lat_point(size_t bytes, int cpu, double target)
{
    lat_job_t j = {bytes, target, 0, cpu};
    pthread_t th;
    if (pthread_create(&th, NULL, lat_worker, &j) != 0)
        bu_die("membench: pthread_create failed");
    pthread_join(th, NULL);
    return j.ns;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static long long mem_available(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    long long kb = -1;
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %lld kB", &kb) == 1)
            break;
    fclose(f);
    return kb > 0 ? kb * 1024 : -1;
}

static void thp_mode(char *buf, size_t n)
{
    char s[256];
    snprintf(buf, n, "unknown");
    if (bu_read_str("/sys/kernel/mm/transparent_hugepage/enabled", s, sizeof s) == 0) {
        char *a = strchr(s, '['), *b = a ? strchr(a, ']') : NULL;
        if (a && b) {
            *b = 0;
            snprintf(buf, n, "%s", a + 1);
        }
    }
}

static void usage(void)
{
    printf("Usage: membench [options]\n"
           "  -s, --size SIZE       size of each of the 3 arrays, e.g. 512M, 2G\n"
           "                        (default: max(4 x total LLC, 256M), capped by RAM)\n"
           "  -t, --threads LIST    thread counts: auto (sweep 1,2,4..cores,cpus), max\n"
           "                        (cores and all CPUs) or a list like 1,8,16\n"
           "  -k, --kernels LIST    read,write,copy,triad (default: all)\n"
           "  -r, --repeat N        repetitions per measurement, best kept (default 5)\n"
           "  -L, --latency         also run the latency test\n"
           "      --latency-only    only run the latency test\n"
           "      --lat-max SIZE    largest latency working set (default: max(4 x LLC, 512M))\n"
           "      --lat-time SEC    timed chasing per working set (default 0.1)\n"
           "      --kv              also print machine readable RESULT lines\n"
           "  -h, --help\n");
}

static void print_lat_size(char *buf, size_t n, size_t bytes)
{
    if (bytes >= (1u << 30) && bytes % (1u << 30) == 0)
        snprintf(buf, n, "%zuG", bytes >> 30);
    else if (bytes >= (1u << 20) && bytes % (1u << 20) == 0)
        snprintf(buf, n, "%zuM", bytes >> 20);
    else if (bytes >= (1u << 20))
        snprintf(buf, n, "%.1fM", (double)bytes / (1 << 20));
    else
        snprintf(buf, n, "%zuK", bytes >> 10);
}

int main(int argc, char **argv)
{
    long long size = 0, lat_max = 0;
    const char *threads_opt = "auto", *kernels = NULL;
    int do_bw = 1, do_lat = 0, kv = 0;
    double lat_time = 0.1;

    static const struct option opts[] = {
        {"size", required_argument, 0, 's'}, {"threads", required_argument, 0, 't'},
        {"kernels", required_argument, 0, 'k'}, {"repeat", required_argument, 0, 'r'},
        {"latency", no_argument, 0, 'L'}, {"latency-only", no_argument, 0, 'O'},
        {"lat-max", required_argument, 0, 'M'}, {"lat-time", required_argument, 0, 'T'},
        {"kv", no_argument, 0, 'K'}, {"help", no_argument, 0, 'h'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "s:t:k:r:Lh", opts, NULL)) != -1) {
        switch (c) {
        case 's':
            if ((size = bu_parse_size(optarg)) <= 0)
                bu_die("membench: bad --size '%s'", optarg);
            break;
        case 't': threads_opt = optarg; break;
        case 'k': kernels = optarg; break;
        case 'r': g_repeat = atoi(optarg) > 0 ? atoi(optarg) : 1; break;
        case 'L': do_lat = 1; break;
        case 'O': do_lat = 1; do_bw = 0; break;
        case 'M':
            if ((lat_max = bu_parse_size(optarg)) <= 0)
                bu_die("membench: bad --lat-max '%s'", optarg);
            break;
        case 'T': lat_time = atof(optarg) > 0 ? atof(optarg) : 0.1; break;
        case 'K': kv = 1; break;
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }
    for (int k = 0; k < NKERNELS; k++)
        g_kernel_on[k] = kernels == NULL || strstr(kernels, g_kernels[k].name) != NULL;

    static int cpus[BU_MAX_CPUS];
    int ncpus = bu_affinity_cpus(cpus, BU_MAX_CPUS);
    bu_cpu_order(cpus, ncpus);
    int cores = bu_count_cores(cpus, ncpus);
    int nodes = bu_count_nodes(cpus, ncpus);
    long long llc = bu_llc_total();
    long long avail = mem_available();
    char thp[64], b1[32], b2[32];
    thp_mode(thp, sizeof thp);

    printf("membench: %d CPUs, %d physical cores, %d NUMA node%s, LLC total %s, "
           "MemAvailable %s, THP %s, SIMD %s\n", ncpus, cores, nodes, nodes == 1 ? "" : "s",
           llc > 0 ? bu_fmt_size(b1, sizeof b1, (double)llc) : "unknown",
           avail > 0 ? bu_fmt_size(b2, sizeof b2, (double)avail) : "unknown", thp, bu_simd_name());

    if (do_bw) {
        int auto_size = size == 0;
        if (auto_size) {
            size = llc > 0 ? 4 * llc : 0;
            if (size < (256ll << 20))
                size = 256ll << 20;
        }
        if (avail > 0 && 3 * size > avail / 2) { /* never use more than half of free RAM */
            long long cap = avail / 6;
            fprintf(stderr, "membench: WARNING: array size reduced to %s to fit in RAM%s\n",
                    bu_fmt_size(b1, sizeof b1, (double)cap),
                    llc > 0 && cap < 4 * llc ? " (smaller than 4x LLC: results may include "
                                               "cache effects)" : "");
            size = cap;
        }
        size_t nelem = round_up((size_t)size / sizeof(double), CHUNK_ALIGN);

        long tlist[MAX_POINTS];
        int npts = 0;
        if (strcmp(threads_opt, "auto") == 0) {
            for (int t = 1; t < cores && npts < MAX_POINTS - 2; t *= 2)
                tlist[npts++] = t;
            tlist[npts++] = cores;
            if (ncpus > cores)
                tlist[npts++] = ncpus;
        } else if (strcmp(threads_opt, "max") == 0) {
            tlist[npts++] = cores;
            if (ncpus > cores)
                tlist[npts++] = ncpus;
        } else {
            npts = bu_parse_int_list(threads_opt, tlist, MAX_POINTS);
            if (npts <= 0)
                bu_die("membench: bad --threads '%s'", threads_opt);
        }
        for (int i = 0; i < npts; i++)
            if (tlist[i] < 1 || tlist[i] > ncpus)
                bu_die("membench: thread count %ld outside 1..%d", tlist[i], ncpus);

        printf("membench: 3 arrays x %s, best of %d, decimal GB/s (1e9 bytes/s)\n\n",
               bu_fmt_size(b1, sizeof b1, (double)(nelem * sizeof(double))), g_repeat);
        printf("%8s", "threads");
        for (int k = 0; k < NKERNELS; k++)
            if (g_kernel_on[k])
                printf(" %10s", g_kernels[k].name);
        printf("\n");
        fflush(stdout);

        double best[NKERNELS] = {0};
        int best_threads[NKERNELS] = {0}, best_method[NKERNELS];
        for (int k = 0; k < NKERNELS; k++)
            best_method[k] = -1;
        for (int i = 0; i < npts; i++) {
            double gbs[NKERNELS];
            int meth[NKERNELS];
            bw_point((int)tlist[i], cpus, nelem, gbs, meth);
            printf("%8ld", tlist[i]);
            for (int k = 0; k < NKERNELS; k++) {
                if (!g_kernel_on[k])
                    continue;
                printf(" %10.2f", gbs[k]);
                if (gbs[k] > best[k]) {
                    best[k] = gbs[k];
                    best_threads[k] = (int)tlist[i];
                    best_method[k] = meth[k];
                }
            }
            printf("\n");
            if (kv)
                for (int k = 0; k < NKERNELS; k++)
                    if (g_kernel_on[k] && meth[k] >= 0)
                        printf("RESULT kind=bw_point kernel=%s threads=%ld value=%.3f unit=GB/s "
                               "method=%s\n", g_kernels[k].name, tlist[i], gbs[k],
                               g_methods[meth[k]].method);
            fflush(stdout);
        }
        printf("%8s", "best");
        for (int k = 0; k < NKERNELS; k++)
            if (g_kernel_on[k])
                printf(" %10.2f", best[k]);
        printf("\n%8s", "@threads");
        for (int k = 0; k < NKERNELS; k++)
            if (g_kernel_on[k])
                printf(" %10d", best_threads[k]);
        printf("\n%8s", "method");
        for (int k = 0; k < NKERNELS; k++)
            if (g_kernel_on[k])
                printf(" %10s", best_method[k] >= 0 ? g_methods[best_method[k]].method : "-");
        printf("\n");
        if (kv)
            for (int k = 0; k < NKERNELS; k++)
                if (g_kernel_on[k] && best_method[k] >= 0)
                    printf("RESULT kind=bw kernel=%s threads=%d value=%.3f unit=GB/s method=%s "
                           "array_bytes=%zu\n", g_kernels[k].name, best_threads[k], best[k],
                           g_methods[best_method[k]].method, nelem * sizeof(double));
        fflush(stdout);
    }

    if (do_lat) {
        int cpu = bu_fastest_cpu(cpus, ncpus);
        long l1 = bu_cache_size(cpu, 1, NULL, 0), l2 = bu_cache_size(cpu, 2, NULL, 0);
        long l3 = bu_cache_size(cpu, 3, NULL, 0);
        if (lat_max == 0) {
            lat_max = llc > 0 ? 4 * llc : 0;
            if (lat_max < (512ll << 20))
                lat_max = 512ll << 20;
        }
        if (avail > 0 && lat_max > avail / 4)
            lat_max = avail / 4;
        if (lat_max > (64ll << 30))
            lat_max = 64ll << 30;

        size_t sizes[256];
        int ns = 0;
        for (size_t s = 4096; s <= (size_t)lat_max && ns < 250; s *= 2) {
            sizes[ns++] = s;
            if (s + s / 2 <= (size_t)lat_max)
                sizes[ns++] = s + s / 2;
        }
        printf("\nmembench: latency, random chase over 64 B lines on CPU %d (ns per load)\n", cpu);
        printf("%10s %10s\n", "size", "ns");
        double *curve = bu_xcalloc((size_t)ns, sizeof *curve);
        for (int i = 0; i < ns; i++) {
            curve[i] = lat_point(sizes[i], cpu, lat_time);
            char sb[32];
            print_lat_size(sb, sizeof sb, sizes[i]);
            printf("%10s %10.2f\n", sb, curve[i]);
            if (kv)
                printf("RESULT kind=lat_point bytes=%zu value=%.3f unit=ns\n", sizes[i], curve[i]);
            fflush(stdout);
        }
        /* level summary from the curve: half of L1, a quarter of L2/L3 (well
         * inside the level, but small enough to keep TLB misses out of the
         * figure) and the largest working set for DRAM */
        const char *names[4] = {"l1", "l2", "l3", "dram"};
        size_t target[4] = {l1 > 0 ? (size_t)l1 / 2 : 0, l2 > 0 ? (size_t)l2 / 4 : 0,
                            l3 > 0 ? (size_t)l3 / 4 : 0, ns > 0 ? sizes[ns - 1] : 0};
        if (l1 > 0 && target[1] < 2 * (size_t)l1)
            target[1] = l2 > 0 ? (size_t)l2 / 2 : 0;
        if (l2 > 0 && target[2] < 2 * (size_t)l2)
            target[2] = l3 > 0 ? (size_t)l3 / 2 : 0;
        size_t prev = 0;
        printf("\n");
        for (int lvl = 0; lvl < 4; lvl++) {
            int pick = -1;
            for (int i = 0; i < ns; i++)
                if (sizes[i] <= target[lvl])
                    pick = i;
            if (target[lvl] < 4096 || pick < 0 || sizes[pick] <= prev)
                continue;
            prev = sizes[pick];
            char sb[32];
            print_lat_size(sb, sizeof sb, sizes[pick]);
            printf("%-5s %8.2f ns  (working set %s)\n", names[lvl], curve[pick], sb);
            if (kv)
                printf("RESULT kind=lat level=%s bytes=%zu value=%.3f unit=ns\n", names[lvl],
                       sizes[pick], curve[pick]);
        }
        free(curve);
        fflush(stdout);
    }
    return 0;
}
