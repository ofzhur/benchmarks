/*
 * httpload - multi-threaded closed-loop HTTP/1.1 load generator.
 *
 * Every thread drives its share of keep-alive connections from its own
 * epoll loop. A connection sends `pipeline` requests at once and the next
 * batch as soon as all responses have arrived (the wrk model). Responses
 * are framed by Content-Length. Latency (batch sent -> response complete)
 * is recorded into a log-linear histogram with < 1% relative error.
 *
 * Author: Oleg Zhurko <oleg@zhurko.de>
 * SPDX-License-Identifier: MIT
 */
#include "benchutil.h"

#include <getopt.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdatomic.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>

#define RBUF 65536
#define MAX_EVENTS 512
#define RETRY_NS 100000000ull /* reconnect back-off after a failed connect */

/* ------------------------------------------------------------------ */
/* histogram: 128 linear sub-buckets per power of two, ns resolution   */
/* ------------------------------------------------------------------ */

#define H_SUB_BITS 7
#define H_SUB (1 << H_SUB_BITS)
#define H_MAX_EXP 40 /* up to ~18 minutes */
#define H_BUCKETS (H_SUB + (H_MAX_EXP - H_SUB_BITS + 1) * H_SUB)

typedef struct {
    uint64_t c[H_BUCKETS];
    uint64_t n, sum, min, max;
} hist_t;

static inline int h_index(uint64_t v)
{
    if (v < H_SUB)
        return (int)v;
    int e = 63 - __builtin_clzll(v);
    if (e > H_MAX_EXP) {
        e = H_MAX_EXP;
        v = (2ull << H_MAX_EXP) - 1;
    }
    return H_SUB + (e - H_SUB_BITS) * H_SUB + (int)(v >> (e - H_SUB_BITS)) - H_SUB;
}

static inline double h_value(int idx) /* bucket midpoint */
{
    if (idx < H_SUB)
        return idx;
    int k = idx - H_SUB, e = k / H_SUB + H_SUB_BITS, sub = k % H_SUB;
    double lo = (double)((uint64_t)(H_SUB + sub) << (e - H_SUB_BITS));
    return lo + (double)(1ull << (e - H_SUB_BITS)) / 2;
}

static inline void hist_add(hist_t *h, uint64_t v)
{
    h->c[h_index(v)]++;
    h->n++;
    h->sum += v;
    if (v < h->min)
        h->min = v;
    if (v > h->max)
        h->max = v;
}

static void hist_merge(hist_t *d, const hist_t *s)
{
    for (int i = 0; i < H_BUCKETS; i++)
        d->c[i] += s->c[i];
    d->n += s->n;
    d->sum += s->sum;
    if (s->min < d->min)
        d->min = s->min;
    if (s->max > d->max)
        d->max = s->max;
}

static double hist_pct(const hist_t *h, double pct)
{
    if (!h->n)
        return 0;
    uint64_t target = (uint64_t)ceil(pct / 100.0 * (double)h->n), cum = 0;
    if (target < 1)
        target = 1;
    for (int i = 0; i < H_BUCKETS; i++) {
        cum += h->c[i];
        if (cum >= target) {
            double v = h_value(i);
            if (v < (double)h->min)
                v = (double)h->min;
            if (v > (double)h->max)
                v = (double)h->max;
            return v;
        }
    }
    return (double)h->max;
}

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

enum { CS_IDLE, CS_CONNECTING, CS_ACTIVE };
enum { PS_HEAD, PS_BODY };
enum { PH_WARMUP, PH_MEASURE, PH_STOP };

typedef struct {
    int fd, state, outstanding, out_armed;
    uint32_t gen; /* bumped on every close: detects stale epoll events */
    size_t woff;
    uint64_t sent_ns, retry_ns;
    /* response parser */
    int pstate, line_no, status, conn_close;
    long long clen;
    size_t body_left, llen;
    char line[128];
} lconn_t;

typedef struct {
    pthread_t th;
    int id, cpu, ep, nconn;
    lconn_t *conns;
    char *rbuf;
    uint64_t requests, bytes, s2xx, sother, e_connect, e_read, e_timeout, e_parse, reconnects;
    hist_t hist;
} lthread_t;

static struct sockaddr_storage g_addr;
static socklen_t g_addrlen;
static char *g_req;
static size_t g_req_len;
static int g_pipeline = 1;
static uint64_t g_timeout_ns = 10000000000ull;
static atomic_int g_phase;

static inline int phase(void) { return atomic_load_explicit(&g_phase, memory_order_relaxed); }

static void ep_set(lthread_t *t, lconn_t *c, uint32_t events, int op)
{
    struct epoll_event ev;
    ev.events = events;
    ev.data.u64 = ((uint64_t)c->gen << 32) | (uint32_t)(c - t->conns);
    epoll_ctl(t->ep, op, c->fd, &ev);
}

static void lc_start(lthread_t *t, lconn_t *c, uint64_t now);

static void lc_close(lthread_t *t, lconn_t *c, uint64_t now, int reconnect)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->gen++;
    c->state = CS_IDLE;
    c->outstanding = 0;
    if (reconnect && phase() != PH_STOP) {
        t->reconnects++;
        lc_start(t, c, now);
    }
}

static void lc_start(lthread_t *t, lconn_t *c, uint64_t now)
{
    c->pstate = PS_HEAD;
    c->line_no = 0;
    c->llen = 0;
    c->outstanding = 0;
    c->woff = 0;
    c->out_armed = 0;
    c->retry_ns = 0;
    int fd = socket(g_addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        t->e_connect++;
        c->retry_ns = now + RETRY_NS;
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, (struct sockaddr *)&g_addr, g_addrlen) < 0 && errno != EINPROGRESS) {
        close(fd);
        t->e_connect++;
        c->retry_ns = now + RETRY_NS;
        return;
    }
    c->fd = fd;
    c->state = CS_CONNECTING;
    c->sent_ns = now; /* connect timeout */
    ep_set(t, c, EPOLLOUT | EPOLLIN, EPOLL_CTL_ADD);
}

/* Writes the rest of the current request batch. -1 if the connection was
 * closed (and restarted). */
static int lc_write(lthread_t *t, lconn_t *c, uint64_t now)
{
    while (c->woff < g_req_len) {
        ssize_t r = send(c->fd, g_req + c->woff, g_req_len - c->woff, MSG_NOSIGNAL);
        if (r > 0) {
            c->woff += (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!c->out_armed) {
                ep_set(t, c, EPOLLIN | EPOLLOUT, EPOLL_CTL_MOD);
                c->out_armed = 1;
            }
            return 0;
        }
        t->e_read++;
        lc_close(t, c, now, 1);
        return -1;
    }
    if (c->out_armed) {
        ep_set(t, c, EPOLLIN, EPOLL_CTL_MOD);
        c->out_armed = 0;
    }
    return 0;
}

static int lc_send_batch(lthread_t *t, lconn_t *c, uint64_t now)
{
    c->woff = 0;
    c->sent_ns = now;
    c->outstanding = g_pipeline;
    return lc_write(t, c, now);
}

/* One response finished. Returns 1 if the connection was closed. */
static int lc_complete(lthread_t *t, lconn_t *c, uint64_t now)
{
    if (phase() == PH_MEASURE) {
        t->requests++;
        if (c->status >= 200 && c->status < 300)
            t->s2xx++;
        else
            t->sother++;
        hist_add(&t->hist, now - c->sent_ns);
    }
    c->pstate = PS_HEAD;
    c->line_no = 0;
    if (--c->outstanding > 0)
        return 0;
    if (c->conn_close) {
        lc_close(t, c, now, 1);
        return 1;
    }
    if (phase() == PH_STOP)
        return 0;
    return lc_send_batch(t, c, now) < 0 ? 1 : 0;
}

/* Processes one header line. Returns -1 on a protocol error, 1 if the
 * connection was closed, 0 otherwise. */
static int lc_header_line(lthread_t *t, lconn_t *c, uint64_t now)
{
    const char *L = c->line;
    if (c->line_no++ == 0) {
        if (c->llen < 12 || strncmp(L, "HTTP/1.", 7) != 0)
            return -1;
        c->status = atoi(L + 9);
        c->clen = -1;
        c->conn_close = L[7] == '0';
        return 0;
    }
    if (L[0] == '\r' || L[0] == '\n') { /* end of headers */
        if (c->status >= 100 && c->status < 200) {
            c->line_no = 0; /* interim response, the real one follows */
            return 0;
        }
        if (c->clen < 0) {
            if (c->status != 204 && c->status != 304)
                return -1; /* chunked or close-delimited bodies are not supported */
            c->clen = 0;
        }
        if (c->clen == 0)
            return lc_complete(t, c, now);
        c->pstate = PS_BODY;
        c->body_left = (size_t)c->clen;
        return 0;
    }
    if (strncasecmp(L, "content-length:", 15) == 0) {
        c->clen = strtoll(L + 15, NULL, 10);
    } else if (strncasecmp(L, "connection:", 11) == 0) {
        if (strcasestr(L + 11, "close"))
            c->conn_close = 1;
        else if (strcasestr(L + 11, "keep-alive"))
            c->conn_close = 0;
    } else if (strncasecmp(L, "transfer-encoding:", 18) == 0) {
        return -1;
    }
    return 0;
}

/* Feeds received bytes into the streaming response parser. */
static int lc_feed(lthread_t *t, lconn_t *c, const char *p, size_t n, uint64_t now)
{
    while (n > 0) {
        if (c->pstate == PS_BODY) {
            size_t k = n < c->body_left ? n : c->body_left;
            c->body_left -= k;
            p += k;
            n -= k;
            if (c->body_left == 0) {
                int rc = lc_complete(t, c, now);
                if (rc)
                    return rc;
            }
            continue;
        }
        const char *nl = memchr(p, '\n', n);
        size_t k = nl ? (size_t)(nl - p) + 1 : n;
        size_t room = sizeof c->line - 1 - c->llen, cp = k < room ? k : room;
        memcpy(c->line + c->llen, p, cp);
        c->llen += cp;
        p += k;
        n -= k;
        if (!nl)
            break;
        c->line[c->llen] = 0;
        int rc = lc_header_line(t, c, now);
        c->llen = 0;
        if (rc)
            return rc;
    }
    return 0;
}

static void lc_read(lthread_t *t, lconn_t *c)
{
    uint32_t gen = c->gen;
    for (;;) {
        ssize_t r = recv(c->fd, t->rbuf, RBUF, 0);
        uint64_t now = bu_now_ns();
        if (r > 0) {
            if (phase() == PH_MEASURE)
                t->bytes += (uint64_t)r;
            int rc = lc_feed(t, c, t->rbuf, (size_t)r, now);
            if (rc < 0) {
                t->e_parse++;
                lc_close(t, c, now, 1);
                return;
            }
            if (rc > 0 || c->gen != gen || r < RBUF)
                return;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (r < 0 || c->outstanding > 0) /* reset, or closed with requests pending */
            t->e_read++;
        lc_close(t, c, now, 1);
        return;
    }
}

static void *lthread_main(void *arg)
{
    lthread_t *t = arg;
    if (t->cpu >= 0)
        bu_pin_self(t->cpu);
    t->ep = epoll_create1(EPOLL_CLOEXEC);
    if (t->ep < 0)
        bu_die("httpload: epoll_create1: %s", strerror(errno));
    t->rbuf = bu_xmalloc(RBUF);
    t->hist.min = UINT64_MAX;
    uint64_t now = bu_now_ns(), last_check = now;
    for (int i = 0; i < t->nconn; i++) {
        t->conns[i].fd = -1;
        lc_start(t, &t->conns[i], now);
    }
    struct epoll_event evs[MAX_EVENTS];
    while (phase() != PH_STOP) {
        int n = epoll_wait(t->ep, evs, MAX_EVENTS, 100);
        if (n < 0 && errno != EINTR)
            bu_die("httpload: epoll_wait: %s", strerror(errno));
        now = bu_now_ns();
        for (int i = 0; i < n; i++) {
            uint32_t idx = (uint32_t)evs[i].data.u64, gen = (uint32_t)(evs[i].data.u64 >> 32);
            lconn_t *c = &t->conns[idx];
            uint32_t e = evs[i].events;
            if (c->gen != gen || c->fd < 0)
                continue; /* event for an already closed socket */
            if (c->state == CS_CONNECTING) {
                int err = 0;
                socklen_t len = sizeof err;
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err || (e & (EPOLLERR | EPOLLHUP))) {
                    t->e_connect++;
                    lc_close(t, c, now, 0);
                    c->retry_ns = now + RETRY_NS;
                    continue;
                }
                if (!(e & EPOLLOUT))
                    continue;
                c->state = CS_ACTIVE;
                ep_set(t, c, EPOLLIN, EPOLL_CTL_MOD);
                if (phase() != PH_STOP)
                    lc_send_batch(t, c, now);
                continue;
            }
            if (e & EPOLLERR) {
                t->e_read++;
                lc_close(t, c, now, 1);
                continue;
            }
            if ((e & EPOLLOUT) && c->out_armed && lc_write(t, c, now) < 0)
                continue;
            if (e & (EPOLLIN | EPOLLHUP))
                lc_read(t, c);
        }
        if (now - last_check > 250000000ull) { /* timeouts and reconnects */
            /* fresh clock: batches sent while handling the events above carry
             * timestamps newer than `now` */
            now = bu_now_ns();
            last_check = now;
            for (int i = 0; i < t->nconn; i++) {
                lconn_t *c = &t->conns[i];
                if (c->state == CS_IDLE) {
                    if (c->retry_ns && now >= c->retry_ns && phase() != PH_STOP)
                        lc_start(t, c, now);
                } else if ((c->state == CS_CONNECTING || c->outstanding > 0) &&
                           (int64_t)(now - c->sent_ns) > (int64_t)g_timeout_ns) {
                    t->e_timeout++;
                    lc_close(t, c, now, 1);
                }
            }
        }
    }
    for (int i = 0; i < t->nconn; i++)
        if (t->conns[i].fd >= 0)
            close(t->conns[i].fd);
    close(t->ep);
    free(t->rbuf);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    printf("Usage: httpload [options] http://host[:port][/path]\n"
           "  -c, --connections N   total keep-alive connections (default 64)\n"
           "  -t, --threads N       threads (default: CPUs in the affinity mask)\n"
           "  -d, --duration SEC    measured time (default 10)\n"
           "  -w, --warmup SEC      warm-up before measuring (default 1)\n"
           "  -p, --pipeline N      requests in flight per connection (default 1)\n"
           "  -H, --header LINE     extra request header, e.g. 'Accept: */*' (repeatable)\n"
           "      --timeout SEC     response timeout (default 10)\n"
           "      --pin             pin thread i to the i-th CPU of the affinity mask\n"
           "      --kv              also print a machine readable RESULT line\n"
           "  -q, --quiet           only print the RESULT line\n"
           "  -h, --help\n"
           "Responses must carry Content-Length (no chunked encoding). GET only.\n");
}

static void fmt_lat(char *buf, size_t n, double ns)
{
    if (ns < 1e3)
        snprintf(buf, n, "%.0fns", ns);
    else if (ns < 1e6)
        snprintf(buf, n, "%.1fus", ns / 1e3);
    else if (ns < 1e9)
        snprintf(buf, n, "%.2fms", ns / 1e6);
    else
        snprintf(buf, n, "%.2fs", ns / 1e9);
}

int main(int argc, char **argv)
{
    int conns = 64, threads = 0, pin = 0, kv = 0, quiet = 0;
    double duration = 10, warmup = 1, timeout = 10;
    char headers[4096] = "";
    size_t hlen = 0;
    static const struct option opts[] = {
        {"connections", required_argument, 0, 'c'}, {"threads", required_argument, 0, 't'},
        {"duration", required_argument, 0, 'd'}, {"warmup", required_argument, 0, 'w'},
        {"pipeline", required_argument, 0, 'p'}, {"header", required_argument, 0, 'H'},
        {"timeout", required_argument, 0, 'T'}, {"pin", no_argument, 0, 'P'},
        {"kv", no_argument, 0, 'K'}, {"quiet", no_argument, 0, 'q'},
        {"help", no_argument, 0, 'h'}, {0, 0, 0, 0}};
    int ch;
    while ((ch = getopt_long(argc, argv, "c:t:d:w:p:H:qh", opts, NULL)) != -1) {
        switch (ch) {
        case 'c': conns = atoi(optarg); break;
        case 't': threads = atoi(optarg); break;
        case 'd': duration = atof(optarg); break;
        case 'w': warmup = atof(optarg); break;
        case 'p': g_pipeline = atoi(optarg); break;
        case 'H': {
            size_t l = strlen(optarg);
            if (hlen + l + 3 > sizeof headers)
                bu_die("httpload: too many headers");
            memcpy(headers + hlen, optarg, l);
            memcpy(headers + hlen + l, "\r\n", 3);
            hlen += l + 2;
            break;
        }
        case 'T': timeout = atof(optarg); break;
        case 'P': pin = 1; break;
        case 'K': kv = 1; break;
        case 'q': quiet = 1; kv = 1; break;
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }
    if (optind != argc - 1) {
        usage();
        return 2;
    }
    if (conns < 1 || g_pipeline < 1 || duration <= 0 || warmup < 0 || timeout <= 0)
        bu_die("httpload: invalid option value");
    g_timeout_ns = (uint64_t)(timeout * 1e9);

    /* URL: http://host[:port][/path] */
    const char *url = argv[optind];
    if (strncmp(url, "http://", 7) != 0)
        bu_die("httpload: only http:// URLs are supported");
    const char *auth = url + 7, *slash = strchr(auth, '/');
    const char *auth_end = slash ? slash : auth + strlen(auth);
    const char *path = slash ? slash : "/";
    char host[256], port[16] = "80", authority[300];
    snprintf(authority, sizeof authority, "%.*s", (int)(auth_end - auth), auth);
    if (*auth == '[') {
        const char *rb = memchr(auth, ']', (size_t)(auth_end - auth));
        if (!rb)
            bu_die("httpload: bad IPv6 address in URL");
        snprintf(host, sizeof host, "%.*s", (int)(rb - auth - 1), auth + 1);
        if (rb[1] == ':')
            snprintf(port, sizeof port, "%.*s", (int)(auth_end - rb - 2), rb + 2);
    } else {
        const char *colon = memchr(auth, ':', (size_t)(auth_end - auth));
        snprintf(host, sizeof host, "%.*s", (int)((colon ? colon : auth_end) - auth), auth);
        if (colon)
            snprintf(port, sizeof port, "%.*s", (int)(auth_end - colon - 1), colon + 1);
    }
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0)
        bu_die("httpload: cannot resolve %s: %s", host, gai_strerror(rc));
    memcpy(&g_addr, res->ai_addr, res->ai_addrlen);
    g_addrlen = res->ai_addrlen;
    freeaddrinfo(res);

    /* request batch */
    char one[8192];
    int ol = snprintf(one, sizeof one, "GET %s HTTP/1.1\r\nHost: %s\r\n%s\r\n", path, authority,
                      headers);
    if (ol <= 0 || (size_t)ol >= sizeof one)
        bu_die("httpload: request too long");
    g_req_len = (size_t)ol * (size_t)g_pipeline;
    g_req = bu_xmalloc(g_req_len);
    for (int i = 0; i < g_pipeline; i++)
        memcpy(g_req + (size_t)i * (size_t)ol, one, (size_t)ol);

    signal(SIGPIPE, SIG_IGN);
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < (rlim_t)conns + 64) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
        getrlimit(RLIMIT_NOFILE, &rl);
        if (rl.rlim_cur < (rlim_t)conns + 64)
            bu_die("httpload: %d connections need 'ulimit -n' >= %d (now %llu)", conns,
                   conns + 64, (unsigned long long)rl.rlim_cur);
    }

    static int cpus[BU_MAX_CPUS];
    int ncpus = bu_affinity_cpus(cpus, BU_MAX_CPUS);
    bu_cpu_order(cpus, ncpus);
    if (threads <= 0)
        threads = ncpus;
    if (threads > conns)
        threads = conns;

    lthread_t *ts = bu_xcalloc((size_t)threads, sizeof *ts);
    atomic_store(&g_phase, PH_WARMUP);
    for (int i = 0; i < threads; i++) {
        ts[i].id = i;
        ts[i].cpu = pin ? cpus[i % ncpus] : -1;
        ts[i].nconn = conns / threads + (i < conns % threads ? 1 : 0);
        ts[i].conns = bu_xcalloc((size_t)ts[i].nconn, sizeof(lconn_t));
        if (pthread_create(&ts[i].th, NULL, lthread_main, &ts[i]) != 0)
            bu_die("httpload: pthread_create failed");
    }
    bu_sleep(warmup);
    double t0 = bu_now();
    atomic_store(&g_phase, PH_MEASURE);
    bu_sleep(duration);
    atomic_store(&g_phase, PH_STOP);
    double t1 = bu_now();
    for (int i = 0; i < threads; i++)
        pthread_join(ts[i].th, NULL);

    hist_t *h = bu_xcalloc(1, sizeof *h);
    h->min = UINT64_MAX;
    uint64_t req = 0, bytes = 0, s2 = 0, so = 0, ec = 0, er = 0, et = 0, ep = 0, rcn = 0;
    for (int i = 0; i < threads; i++) {
        hist_merge(h, &ts[i].hist);
        req += ts[i].requests;
        bytes += ts[i].bytes;
        s2 += ts[i].s2xx;
        so += ts[i].sother;
        ec += ts[i].e_connect;
        er += ts[i].e_read;
        et += ts[i].e_timeout;
        ep += ts[i].e_parse;
        rcn += ts[i].reconnects;
    }
    double secs = t1 - t0, rps = (double)req / secs, mbps = (double)bytes / secs / 1e6;
    double avg = h->n ? (double)h->sum / (double)h->n : 0;
    double p50 = hist_pct(h, 50), p90 = hist_pct(h, 90), p99 = hist_pct(h, 99);
    double p999 = hist_pct(h, 99.9), mx = h->n ? (double)h->max : 0;

    if (!quiet) {
        char a[32], b[32], c[32], d[32], e[32], f[32];
        fmt_lat(a, sizeof a, avg);
        fmt_lat(b, sizeof b, p50);
        fmt_lat(c, sizeof c, p90);
        fmt_lat(d, sizeof d, p99);
        fmt_lat(e, sizeof e, p999);
        fmt_lat(f, sizeof f, mx);
        printf("httpload: %s  threads=%d connections=%d pipeline=%d\n", url, threads, conns,
               g_pipeline);
        printf("  requests  %llu in %.2fs = %.1f req/s\n", (unsigned long long)req, secs, rps);
        printf("  transfer  %.2f MB/s\n", mbps);
        printf("  latency   avg %s  p50 %s  p90 %s  p99 %s  p99.9 %s  max %s\n", a, b, c, d, e,
               f);
        printf("  errors    connect %llu  read %llu  timeout %llu  parse %llu  non-2xx %llu  "
               "reconnects %llu\n", (unsigned long long)ec, (unsigned long long)er,
               (unsigned long long)et, (unsigned long long)ep, (unsigned long long)so,
               (unsigned long long)rcn);
    }
    if (kv)
        printf("RESULT rps=%.2f requests=%llu seconds=%.3f mbps=%.3f lat_avg_us=%.2f "
               "lat_p50_us=%.2f lat_p90_us=%.2f lat_p99_us=%.2f lat_p999_us=%.2f "
               "lat_max_us=%.2f ok=%llu non2xx=%llu err_connect=%llu err_read=%llu "
               "err_timeout=%llu err_parse=%llu reconnects=%llu threads=%d connections=%d "
               "pipeline=%d\n", rps, (unsigned long long)req, secs, mbps, avg / 1e3, p50 / 1e3,
               p90 / 1e3, p99 / 1e3, p999 / 1e3, mx / 1e3, (unsigned long long)s2,
               (unsigned long long)so, (unsigned long long)ec, (unsigned long long)er,
               (unsigned long long)et, (unsigned long long)ep, (unsigned long long)rcn, threads,
               conns, g_pipeline);
    return req > 0 ? 0 : 1;
}
