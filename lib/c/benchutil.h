/*
 * benchutil.h - small header-only helpers shared by the C benchmarks:
 * timing, CPU affinity, CPU topology ordering, cache detection, parsing.
 *
 * Author: Oleg Zhurko <oleg@zhurko.de>
 * SPDX-License-Identifier: MIT
 */
#ifndef BENCHUTIL_H
#define BENCHUTIL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BU_MAX_CPUS 8192

#if defined(__GNUC__)
#define BU_UNUSED __attribute__((unused))
#else
#define BU_UNUSED
#endif

/* ------------------------------------------------------------------ */
/* errors, time                                                        */
/* ------------------------------------------------------------------ */

static BU_UNUSED void bu_die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static inline uint64_t bu_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline double bu_now(void) { return (double)bu_now_ns() * 1e-9; }

static BU_UNUSED void bu_sleep(double sec)
{
    struct timespec ts;
    if (sec <= 0)
        return;
    ts.tv_sec = (time_t)sec;
    ts.tv_nsec = (long)((sec - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

static BU_UNUSED void *bu_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        bu_die("out of memory (%zu bytes)", n);
    return p;
}

static BU_UNUSED void *bu_xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p)
        bu_die("out of memory (%zu x %zu bytes)", n, sz);
    return p;
}

/* 64-byte aligned allocation (cache line), zeroed. */
static BU_UNUSED void *bu_aligned_alloc(size_t n)
{
    void *p = NULL;
    if (posix_memalign(&p, 64, n ? n : 64) != 0)
        bu_die("out of memory (%zu bytes)", n);
    memset(p, 0, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* portable barrier (pthread_barrier_t is missing on some platforms)   */
/* ------------------------------------------------------------------ */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned count, waiting;
    unsigned long phase;
} bu_barrier_t;

static BU_UNUSED void bu_barrier_init(bu_barrier_t *b, unsigned count)
{
    pthread_mutex_init(&b->mu, NULL);
    pthread_cond_init(&b->cv, NULL);
    b->count = count;
    b->waiting = 0;
    b->phase = 0;
}

static BU_UNUSED void bu_barrier_destroy(bu_barrier_t *b)
{
    pthread_mutex_destroy(&b->mu);
    pthread_cond_destroy(&b->cv);
}

static BU_UNUSED void bu_barrier_wait(bu_barrier_t *b)
{
    pthread_mutex_lock(&b->mu);
    unsigned long phase = b->phase;
    if (++b->waiting == b->count) {
        b->waiting = 0;
        b->phase++;
        pthread_cond_broadcast(&b->cv);
    } else {
        while (phase == b->phase)
            pthread_cond_wait(&b->cv, &b->mu);
    }
    pthread_mutex_unlock(&b->mu);
}

/* ------------------------------------------------------------------ */
/* sysfs helpers                                                       */
/* ------------------------------------------------------------------ */

static BU_UNUSED int bu_read_str(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}

static BU_UNUSED long bu_read_long(const char *path, long dflt)
{
    char buf[64];
    if (bu_read_str(path, buf, sizeof buf) != 0)
        return dflt;
    char *end;
    long v = strtol(buf, &end, 10);
    return end == buf ? dflt : v;
}

/* "64K", "2M", "1.5G", "123" (bytes; binary multiples). -1 on error. */
static BU_UNUSED long long bu_parse_size(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || v < 0)
        return -1;
    while (*end == ' ')
        end++;
    double mul = 1;
    switch (toupper((unsigned char)*end)) {
    case 'K': mul = 1024.0; break;
    case 'M': mul = 1024.0 * 1024; break;
    case 'G': mul = 1024.0 * 1024 * 1024; break;
    case 'T': mul = 1024.0 * 1024 * 1024 * 1024; break;
    case 0: case 'B': break;
    default: return -1;
    }
    return (long long)(v * mul);
}

/* Human readable binary size, e.g. "256 MiB". */
static BU_UNUSED const char *bu_fmt_size(char *buf, size_t n, double bytes)
{
    const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int i = 0;
    while (bytes >= 1024 && i < 4) {
        bytes /= 1024;
        i++;
    }
    if (bytes == (double)(long long)bytes)
        snprintf(buf, n, "%lld %s", (long long)bytes, u[i]);
    else
        snprintf(buf, n, "%.1f %s", bytes, u[i]);
    return buf;
}

/* Parses a comma separated list of positive integers. Returns count. */
static BU_UNUSED int bu_parse_int_list(const char *s, long *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s)
            return -1;
        out[n++] = v;
        s = end;
        while (*s == ',' || *s == ' ')
            s++;
    }
    return n;
}

/* Parses a kernel cpulist ("0-3,8,10-11"). Returns count. */
static BU_UNUSED int bu_parse_cpulist(const char *s, int *out, int max)
{
    int n = 0;
    while (*s) {
        char *end;
        long a = strtol(s, &end, 10), b;
        if (end == s)
            break;
        b = a;
        s = end;
        if (*s == '-') {
            b = strtol(s + 1, &end, 10);
            s = end;
        }
        for (long c = a; c <= b && n < max; c++)
            out[n++] = (int)c;
        while (*s == ',' || *s == ' ' || *s == '\n')
            s++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* affinity                                                            */
/* ------------------------------------------------------------------ */

/* CPUs this process may run on (sched_getaffinity), ascending. */
static BU_UNUSED int bu_affinity_cpus(int *cpus, int max)
{
    int n = 0;
#if defined(__linux__)
    for (int ncpu = 1024; ncpu <= BU_MAX_CPUS * 8; ncpu *= 2) {
        cpu_set_t *set = CPU_ALLOC(ncpu);
        size_t sz = CPU_ALLOC_SIZE(ncpu);
        if (!set)
            break;
        CPU_ZERO_S(sz, set);
        if (sched_getaffinity(0, sz, set) == 0) {
            for (int c = 0; c < ncpu && n < max; c++)
                if (CPU_ISSET_S(c, sz, set))
                    cpus[n++] = c;
            CPU_FREE(set);
            return n;
        }
        CPU_FREE(set);
        if (errno != EINVAL)
            break;
    }
#endif
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online < 1)
        online = 1;
    for (n = 0; n < online && n < max; n++)
        cpus[n] = n;
    return n;
}

/* Pins the calling thread to one CPU. Returns 0 on success. */
static BU_UNUSED int bu_pin_self(int cpu)
{
#if defined(__linux__)
    if (cpu < 0)
        return -1;
    cpu_set_t *set = CPU_ALLOC(cpu + 1);
    size_t sz = CPU_ALLOC_SIZE(cpu + 1);
    if (!set)
        return -1;
    CPU_ZERO_S(sz, set);
    CPU_SET_S(cpu, sz, set);
    int rc = pthread_setaffinity_np(pthread_self(), sz, set);
    CPU_FREE(set);
    return rc;
#else
    (void)cpu;
    return -1;
#endif
}

/* ------------------------------------------------------------------ */
/* topology                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int cpu;
    int node;  /* NUMA node */
    int smt;   /* index of this CPU among its core's hardware threads */
    int core;  /* first CPU of the core (identifies the physical core) */
} bu_topo_t;

static BU_UNUSED int bu_cpu_node(int cpu)
{
#if defined(__linux__)
    char path[96];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d", cpu);
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, "node", 4) == 0 && isdigit((unsigned char)e->d_name[4])) {
                int node = atoi(e->d_name + 4);
                closedir(d);
                return node;
            }
        }
        closedir(d);
    }
#else
    (void)cpu;
#endif
    return 0;
}

static BU_UNUSED void bu_cpu_topo(int cpu, bu_topo_t *t)
{
    char path[128], buf[1024];
    int sib[256];
    t->cpu = cpu;
    t->node = bu_cpu_node(cpu);
    t->smt = 0;
    t->core = cpu;
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
    if (bu_read_str(path, buf, sizeof buf) == 0) {
        int n = bu_parse_cpulist(buf, sib, 256);
        if (n > 0) {
            t->core = sib[0];
            for (int i = 0; i < n; i++)
                if (sib[i] == cpu)
                    t->smt = i;
        }
    }
}

static int bu_topo_cmp(const void *a, const void *b)
{
    const bu_topo_t *x = a, *y = b;
    if (x->smt != y->smt)
        return x->smt - y->smt;
    if (x->node != y->node)
        return x->node - y->node;
    if (x->core != y->core)
        return x->core - y->core;
    return x->cpu - y->cpu;
}

/*
 * Reorders cpus[] so that taking the first k entries spreads k threads as
 * widely as possible: first one hardware thread of every physical core,
 * round-robin across NUMA nodes, then the SMT siblings in the same way.
 */
static BU_UNUSED void bu_cpu_order(int *cpus, int n)
{
    if (n <= 1)
        return;
    bu_topo_t *t = bu_xcalloc((size_t)n, sizeof *t);
    int *start = bu_xcalloc((size_t)n, sizeof *start);
    int *end = bu_xcalloc((size_t)n, sizeof *end);
    for (int i = 0; i < n; i++)
        bu_cpu_topo(cpus[i], &t[i]);
    qsort(t, (size_t)n, sizeof *t, bu_topo_cmp);
    int out = 0;
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && t[j].smt == t[i].smt)
            j++;
        int nn = 0; /* node groups within [i, j) */
        for (int k = i; k < j; k++) {
            if (k == i || t[k].node != t[k - 1].node) {
                start[nn] = k;
                nn++;
            }
            end[nn - 1] = k + 1;
        }
        for (int progress = 1; progress;) {
            progress = 0;
            for (int g = 0; g < nn; g++)
                if (start[g] < end[g]) {
                    cpus[out++] = t[start[g]++].cpu;
                    progress = 1;
                }
        }
        i = j;
    }
    free(t);
    free(start);
    free(end);
}

/*
 * The CPU of cpus[] with the highest single-thread capacity (hybrid and
 * big.LITTLE aware): highest cpu_capacity if the kernel exposes it, else
 * highest cpuinfo_max_freq ("favored" turbo cores), else the first CPU.
 */
static BU_UNUSED int bu_fastest_cpu(const int *cpus, int n)
{
    int best = n > 0 ? cpus[0] : 0;
    long bestv = -1;
    for (int i = 0; i < n; i++) {
        char path[128];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpus[i]);
        long v = bu_read_long(path, -1);
        if (v < 0) {
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",
                     cpus[i]);
            v = bu_read_long(path, -1);
        }
        if (v > bestv) {
            bestv = v;
            best = cpus[i];
        }
    }
    return best;
}

/* Number of distinct physical cores among cpus[]. */
static BU_UNUSED int bu_count_cores(const int *cpus, int n)
{
    int cores = 0;
    for (int i = 0; i < n; i++) {
        bu_topo_t t;
        bu_cpu_topo(cpus[i], &t);
        if (t.smt == 0)
            cores++;
    }
    return cores > 0 ? cores : n;
}

/* Number of distinct NUMA nodes among cpus[]. */
static BU_UNUSED int bu_count_nodes(const int *cpus, int n)
{
    int seen[64] = {0}, count = 0;
    for (int i = 0; i < n; i++) {
        int node = bu_cpu_node(cpus[i]);
        if (node >= 0 && node < 64 && !seen[node]) {
            seen[node] = 1;
            count++;
        }
    }
    return count > 0 ? count : 1;
}

/* ------------------------------------------------------------------ */
/* caches                                                              */
/* ------------------------------------------------------------------ */

/*
 * Size in bytes of the data/unified cache of the given level seen by `cpu`
 * (level 1 = L1d). Returns -1 if unknown. *shared_list receives the
 * shared_cpu_list string when non-NULL.
 */
static BU_UNUSED long bu_cache_size(int cpu, int level, char *shared_list, size_t n)
{
    char path[160], buf[256];
    for (int idx = 0; idx < 16; idx++) {
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/level", cpu, idx);
        long lv = bu_read_long(path, -1);
        if (lv < 0)
            break;
        if (lv != level)
            continue;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/type", cpu, idx);
        if (bu_read_str(path, buf, sizeof buf) != 0 || strcmp(buf, "Instruction") == 0)
            continue;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/size", cpu, idx);
        if (bu_read_str(path, buf, sizeof buf) != 0)
            continue;
        long long sz = bu_parse_size(buf);
        if (shared_list) {
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/shared_cpu_list", cpu, idx);
            if (bu_read_str(path, shared_list, n) != 0)
                shared_list[0] = 0;
        }
        return sz > 0 ? (long)sz : -1;
    }
#if defined(_SC_LEVEL1_DCACHE_SIZE) && defined(_SC_LEVEL2_CACHE_SIZE) && defined(_SC_LEVEL3_CACHE_SIZE)
    if (cpu == 0) {
        long v = -1;
        if (level == 1) v = sysconf(_SC_LEVEL1_DCACHE_SIZE);
        if (level == 2) v = sysconf(_SC_LEVEL2_CACHE_SIZE);
        if (level == 3) v = sysconf(_SC_LEVEL3_CACHE_SIZE);
        if (shared_list)
            shared_list[0] = 0;
        return v > 0 ? v : -1;
    }
#endif
    return -1;
}

/* Highest cache level present for cpu 0 (data or unified), 0 if unknown. */
static BU_UNUSED int bu_llc_level(void)
{
    int best = 0;
    for (int lv = 1; lv <= 4; lv++)
        if (bu_cache_size(0, lv, NULL, 0) > 0)
            best = lv;
    return best;
}

/*
 * Total size of all last-level cache instances in the system (e.g. two
 * sockets with 32 MiB L3 each -> 64 MiB). Returns -1 if unknown.
 */
static BU_UNUSED long long bu_llc_total(void)
{
    int level = bu_llc_level();
    if (level == 0)
        return -1;
    int ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (ncpu < 1)
        ncpu = 1;
    if (ncpu > BU_MAX_CPUS)
        ncpu = BU_MAX_CPUS;
    char *covered = bu_xcalloc((size_t)ncpu, 1);
    long long total = 0;
    int list[BU_MAX_CPUS];
    char shared[1024];
    for (int cpu = 0; cpu < ncpu; cpu++) {
        if (covered[cpu])
            continue;
        long sz = bu_cache_size(cpu, level, shared, sizeof shared);
        if (sz <= 0)
            continue;
        total += sz;
        int n = shared[0] ? bu_parse_cpulist(shared, list, BU_MAX_CPUS) : 0;
        for (int i = 0; i < n; i++)
            if (list[i] >= 0 && list[i] < ncpu)
                covered[list[i]] = 1;
        covered[cpu] = 1;
    }
    free(covered);
    return total > 0 ? total : -1;
}

/* ------------------------------------------------------------------ */
/* misc                                                                */
/* ------------------------------------------------------------------ */

/* Short description of the SIMD instruction set the binary was built for. */
static BU_UNUSED const char *bu_simd_name(void)
{
#if defined(__AVX512F__)
    return "AVX-512";
#elif defined(__AVX2__)
    return "AVX2";
#elif defined(__AVX__)
    return "AVX";
#elif defined(__SSE4_2__)
    return "SSE4.2";
#elif defined(__SSE2__)
    return "SSE2";
#elif defined(__ARM_FEATURE_SVE)
    return "SVE (NEON kernels)";
#elif defined(__ARM_NEON) || defined(__aarch64__)
    return "NEON";
#elif defined(__riscv_vector)
    return "RVV";
#else
    return "generic";
#endif
}

static BU_UNUSED const char *bu_compiler(void)
{
#if defined(__clang__)
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#else
    return "unknown compiler";
#endif
}

/* xorshift64* PRNG */
static inline uint64_t bu_rand64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

#endif /* BENCHUTIL_H */
