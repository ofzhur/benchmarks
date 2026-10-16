/*
 * httpd - minimal multi-threaded HTTP/1.1 server for request-rate benchmarks.
 *
 * Shared-nothing design: one worker thread per CPU, each with its own
 * SO_REUSEPORT listening socket (the kernel spreads new connections), its
 * own epoll loop and no locks. Keep-alive and pipelining are supported; all
 * responses produced from one read are sent with a single write.
 *
 * Endpoints (GET or HEAD):
 *   /plain, /            "Hello, World!" as text/plain
 *   /json?items=N        N mock user records (default 20, ~180 bytes each)
 *                        serialized to JSON for every request
 *   /compute?rounds=N    SHA-256 chained N times (default 1000) over a
 *                        per-request seed: a CPU-bound handler
 *
 * Author: Oleg Zhurko <oleg@zhurko.de>
 * SPDX-License-Identifier: MIT
 */
#include "benchutil.h"
#include "sha256.h"

#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>

#define RBUF_SIZE 8192
#define MAX_EVENTS 256
#define N_RECORDS 4096
#define MAX_ITEMS 10000
#define DEFAULT_ITEMS 20
#define DEFAULT_ROUNDS 1000
#define MAX_ROUNDS 10000000

/* ------------------------------------------------------------------ */
/* growable output buffer                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char *p;
    size_t len, cap;
} buf_t;

static void buf_reserve(buf_t *b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra)
        cap *= 2;
    char *np = realloc(b->p, cap);
    if (!np)
        bu_die("httpd: out of memory");
    b->p = np;
    b->cap = cap;
}

static inline void buf_put(buf_t *b, const void *s, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

#define BUF_LIT(b, s) buf_put((b), (s), sizeof(s) - 1)

static inline void buf_putc(buf_t *b, char c)
{
    buf_reserve(b, 1);
    b->p[b->len++] = c;
}

static void buf_u64(buf_t *b, uint64_t v)
{
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    buf_reserve(b, (size_t)n);
    while (n)
        b->p[b->len++] = tmp[--n];
}

/* v / 10^digits printed with exactly `digits` decimals (no printf) */
static void buf_fixed(buf_t *b, int64_t v, int digits)
{
    uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v, div = 1;
    if (v < 0)
        buf_putc(b, '-');
    for (int i = 0; i < digits; i++)
        div *= 10;
    buf_u64(b, u / div);
    buf_putc(b, '.');
    uint64_t frac = u % div;
    char tmp[20];
    for (int i = digits - 1; i >= 0; i--) {
        tmp[i] = (char)('0' + frac % 10);
        frac /= 10;
    }
    buf_put(b, tmp, (size_t)digits);
}

static void buf_json_str(buf_t *b, const char *s)
{
    const char *run = s;
    buf_putc(b, '"');
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch != '"' && ch != '\\' && ch >= 0x20)
            continue;
        buf_put(b, run, (size_t)(s - run));
        if (ch == '"') {
            BUF_LIT(b, "\\\"");
        } else if (ch == '\\') {
            BUF_LIT(b, "\\\\");
        } else {
            char e[8];
            snprintf(e, sizeof e, "\\u%04x", ch);
            buf_put(b, e, 6);
        }
        run = s + 1;
    }
    buf_put(b, run, (size_t)(s - run));
    buf_putc(b, '"');
}

/* ------------------------------------------------------------------ */
/* mock data                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t id, created;
    int64_t balance_cents;
    int32_t score_milli;
    uint8_t age, active, ntags;
    const char *tags[3];
    const char *city;
    char name[48];
    char email[80];
} record_t;

static record_t *g_rec;

static void make_records(void)
{
    static const char *first[32] = {
        "Anna", "Ben", "Clara", "David", "Emma", "Felix", "Greta", "Hugo", "Ida", "Jonas", "Klara",
        "Leon", "Mia", "Noah", "Olga", "Paul", "Quinn", "Rosa", "Sven", "Tara", "Uwe", "Vera",
        "Willi", "Xenia", "Yusuf", "Zoe", "Lukas", "Sofia", "Elias", "Lina", "Max", "Nora"};
    static const char *last[32] = {
        "Schmidt", "Mueller", "Schneider", "Fischer", "Weber", "Meyer", "Wagner", "Becker",
        "Schulz", "Hoffmann", "Koch", "Richter", "Klein", "Wolf", "Neumann", "Schwarz",
        "Zimmermann", "Braun", "Krueger", "Hofmann", "Hartmann", "Lange", "Schmitt", "Werner",
        "Krause", "Meier", "Lehmann", "Schmid", "Schulze", "Maier", "Koehler", "Herrmann"};
    static const char *cities[16] = {"Berlin", "Hamburg", "Munich", "Cologne", "Frankfurt",
                                     "Stuttgart", "Dusseldorf", "Leipzig", "Dortmund", "Essen",
                                     "Bremen", "Dresden", "Hanover", "Nuremberg", "Duisburg",
                                     "Bochum"};
    static const char *tags[12] = {"admin", "beta", "premium", "staff", "trial", "verified",
                                   "vip", "newsletter", "mobile", "api", "support", "partner"};
    uint64_t r = 42;
    g_rec = bu_xcalloc(N_RECORDS, sizeof *g_rec);
    for (int i = 0; i < N_RECORDS; i++) {
        record_t *x = &g_rec[i];
        const char *f = first[bu_rand64(&r) % 32], *l = last[bu_rand64(&r) % 32];
        x->id = 100000u + (uint32_t)i;
        snprintf(x->name, sizeof x->name, "%s %s", f, l);
        snprintf(x->email, sizeof x->email, "%s.%s%u@example.com", f, l,
                 (unsigned)(bu_rand64(&r) % 1000));
        for (char *p = x->email; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        x->age = (uint8_t)(18 + bu_rand64(&r) % 60);
        x->active = bu_rand64(&r) % 4 != 0;
        x->balance_cents = (int64_t)(bu_rand64(&r) % 2000000) - 100000;
        x->score_milli = (int32_t)(bu_rand64(&r) % 100000);
        x->created = 1600000000u + (uint32_t)(bu_rand64(&r) % 150000000u);
        x->city = cities[bu_rand64(&r) % 16];
        x->ntags = (uint8_t)(bu_rand64(&r) % 4);
        for (int t = 0; t < x->ntags; t++)
            x->tags[t] = tags[bu_rand64(&r) % 12];
    }
}

static void json_record(buf_t *b, const record_t *x)
{
    BUF_LIT(b, "{\"id\":");
    buf_u64(b, x->id);
    BUF_LIT(b, ",\"name\":");
    buf_json_str(b, x->name);
    BUF_LIT(b, ",\"email\":");
    buf_json_str(b, x->email);
    BUF_LIT(b, ",\"age\":");
    buf_u64(b, x->age);
    if (x->active)
        BUF_LIT(b, ",\"active\":true");
    else
        BUF_LIT(b, ",\"active\":false");
    BUF_LIT(b, ",\"balance\":");
    buf_fixed(b, x->balance_cents, 2);
    BUF_LIT(b, ",\"score\":");
    buf_fixed(b, x->score_milli, 3);
    BUF_LIT(b, ",\"city\":");
    buf_json_str(b, x->city);
    BUF_LIT(b, ",\"tags\":[");
    for (int t = 0; t < x->ntags; t++) {
        if (t)
            buf_putc(b, ',');
        buf_json_str(b, x->tags[t]);
    }
    BUF_LIT(b, "],\"created\":");
    buf_u64(b, x->created);
    buf_putc(b, '}');
}

/* ------------------------------------------------------------------ */
/* connections and workers                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int fd;
    uint32_t rlen;
    uint8_t closing;   /* close after the pending output is flushed */
    uint8_t out_armed; /* waiting for EPOLLOUT, reading paused */
    buf_t out;
    size_t woff;
    char rbuf[RBUF_SIZE];
} conn_t;

typedef struct {
    pthread_t th;
    int id, cpu, lfd, ep, spare_fd;
    time_t date_sec;
    char date[64];
    size_t date_len;
    uint64_t nreq;
    buf_t body; /* scratch buffer for generated bodies */
} worker_t;

typedef struct {
    const char *method, *path, *query;
    size_t mlen, plen, qlen;
    int keepalive, http10;
} req_t;

static int g_quiet;

static void update_date(worker_t *w)
{
    time_t now = time(NULL);
    if (now == w->date_sec)
        return;
    struct tm tm;
    gmtime_r(&now, &tm);
    w->date_sec = now;
    w->date_len = strftime(w->date, sizeof w->date, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static void conn_close(worker_t *w, conn_t *c)
{
    (void)w;
    close(c->fd); /* also removes it from the epoll set */
    free(c->out.p);
    free(c);
}

/* Returns -1 if the connection was closed, 0 otherwise. */
static int conn_flush(worker_t *w, conn_t *c)
{
    while (c->woff < c->out.len) {
        ssize_t r = send(c->fd, c->out.p + c->woff, c->out.len - c->woff, MSG_NOSIGNAL);
        if (r > 0) {
            c->woff += (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!c->out_armed) {
                struct epoll_event ev = {.events = EPOLLOUT, .data.ptr = c};
                epoll_ctl(w->ep, EPOLL_CTL_MOD, c->fd, &ev);
                c->out_armed = 1;
            }
            return 0;
        }
        conn_close(w, c);
        return -1;
    }
    c->out.len = 0;
    c->woff = 0;
    if (c->out.cap > (1u << 20)) { /* do not keep huge buffers around */
        free(c->out.p);
        c->out.p = NULL;
        c->out.cap = 0;
    }
    if (c->closing) {
        conn_close(w, c);
        return -1;
    }
    if (c->out_armed) {
        struct epoll_event ev = {.events = EPOLLIN, .data.ptr = c};
        epoll_ctl(w->ep, EPOLL_CTL_MOD, c->fd, &ev);
        c->out_armed = 0;
    }
    return 0;
}

static const char *memmem_ci(const char *h, size_t hn, const char *n)
{
    size_t nn = strlen(n);
    for (size_t i = 0; i + nn <= hn; i++)
        if (strncasecmp(h + i, n, nn) == 0)
            return h + i;
    return NULL;
}

/* Bytes of one complete request at p (headers + body), 0 if incomplete,
 * -1 if malformed, -2 if it cannot fit into the receive buffer. */
static long parse_request(const char *p, size_t n, req_t *rq)
{
    const char *end = memmem(p, n, "\r\n\r\n", 4);
    if (!end)
        return n >= RBUF_SIZE ? -2 : 0;
    size_t hlen = (size_t)(end - p) + 4;
    const char *le = memchr(p, '\r', hlen); /* end of the request line */
    const char *sp1 = memchr(p, ' ', (size_t)(le - p));
    if (!sp1)
        return -1;
    const char *tgt = sp1 + 1;
    const char *sp2 = memchr(tgt, ' ', (size_t)(le - tgt));
    if (!sp2)
        return -1;
    const char *q = memchr(tgt, '?', (size_t)(sp2 - tgt));
    rq->method = p;
    rq->mlen = (size_t)(sp1 - p);
    rq->path = tgt;
    rq->plen = (size_t)((q ? q : sp2) - tgt);
    rq->query = q ? q + 1 : NULL;
    rq->qlen = q ? (size_t)(sp2 - q - 1) : 0;
    const char *ver = sp2 + 1;
    if (le - ver != 8 || memcmp(ver, "HTTP/1.", 7) != 0 || (ver[7] != '0' && ver[7] != '1'))
        return -1;
    rq->http10 = ver[7] == '0';
    rq->keepalive = !rq->http10;

    size_t clen = 0;
    for (const char *h = le + 2; h < end;) {
        const char *e = memchr(h, '\r', (size_t)(end + 1 - h));
        if (!e)
            e = end;
        size_t L = (size_t)(e - h);
        if (L > 11 && strncasecmp(h, "connection:", 11) == 0) {
            if (memmem_ci(h + 11, L - 11, "close"))
                rq->keepalive = 0;
            else if (memmem_ci(h + 11, L - 11, "keep-alive"))
                rq->keepalive = 1;
        } else if (L > 15 && strncasecmp(h, "content-length:", 15) == 0) {
            clen = strtoul(h + 15, NULL, 10);
        } else if (L > 18 && strncasecmp(h, "transfer-encoding:", 18) == 0) {
            return -1; /* chunked request bodies are not supported */
        }
        h = e + 2;
    }
    if (hlen + clen > RBUF_SIZE)
        return -2;
    if (n < hlen + clen)
        return 0;
    return (long)(hlen + clen);
}

static long query_long(const req_t *rq, const char *key, long dflt)
{
    size_t klen = strlen(key);
    const char *q = rq->query, *e = q ? q + rq->qlen : NULL;
    while (q && q < e) {
        const char *amp = memchr(q, '&', (size_t)(e - q));
        const char *pe = amp ? amp : e;
        if ((size_t)(pe - q) > klen && memcmp(q, key, klen) == 0 && q[klen] == '=') {
            char tmp[32];
            size_t n = (size_t)(pe - q) - klen - 1;
            if (n >= sizeof tmp)
                n = sizeof tmp - 1;
            memcpy(tmp, q + klen + 1, n);
            tmp[n] = 0;
            char *end;
            long v = strtol(tmp, &end, 10);
            return end == tmp ? dflt : v;
        }
        q = amp ? amp + 1 : e;
    }
    return dflt;
}

static int path_is(const req_t *rq, const char *s)
{
    size_t n = strlen(s);
    return rq->plen == n && memcmp(rq->path, s, n) == 0;
}

static void respond(worker_t *w, conn_t *c, const char *status, size_t slen, const char *ctype,
                    size_t ctlen, const char *body, size_t blen, int head, const req_t *rq)
{
    buf_t *o = &c->out;
    buf_put(o, status, slen);
    BUF_LIT(o, "Server: bench-httpd\r\nDate: ");
    buf_put(o, w->date, w->date_len);
    BUF_LIT(o, "\r\nContent-Type: ");
    buf_put(o, ctype, ctlen);
    BUF_LIT(o, "\r\nContent-Length: ");
    buf_u64(o, blen);
    if (!rq || !rq->keepalive)
        BUF_LIT(o, "\r\nConnection: close");
    else if (rq->http10)
        BUF_LIT(o, "\r\nConnection: keep-alive");
    BUF_LIT(o, "\r\n\r\n");
    if (!head)
        buf_put(o, body, blen);
}

#define RESPOND(w, c, st, ct, body, blen, head, rq) \
    respond((w), (c), st, sizeof(st) - 1, ct, sizeof(ct) - 1, (body), (blen), (head), (rq))

static void handle(worker_t *w, conn_t *c, const req_t *rq)
{
    static const char hello[] = "Hello, World!";
    int head = rq->mlen == 4 && memcmp(rq->method, "HEAD", 4) == 0;
    int get = rq->mlen == 3 && memcmp(rq->method, "GET", 3) == 0;
    w->nreq++;
    if (!get && !head) {
        RESPOND(w, c, "HTTP/1.1 405 Method Not Allowed\r\n", "text/plain", "Method Not Allowed",
                18, 0, rq);
        return;
    }
    if (path_is(rq, "/plain") || path_is(rq, "/") || path_is(rq, "/plaintext")) {
        RESPOND(w, c, "HTTP/1.1 200 OK\r\n", "text/plain", hello, sizeof hello - 1, head, rq);
    } else if (path_is(rq, "/json")) {
        long items = query_long(rq, "items", DEFAULT_ITEMS);
        if (items < 0)
            items = 0;
        if (items > MAX_ITEMS)
            items = MAX_ITEMS;
        buf_t *b = &w->body;
        size_t start = (size_t)(w->nreq * 7919u) % N_RECORDS;
        b->len = 0;
        BUF_LIT(b, "{\"page\":");
        buf_u64(b, w->nreq % 100 + 1);
        BUF_LIT(b, ",\"count\":");
        buf_u64(b, (uint64_t)items);
        BUF_LIT(b, ",\"items\":[");
        for (long i = 0; i < items; i++) {
            if (i)
                buf_putc(b, ',');
            json_record(b, &g_rec[(start + (size_t)i) % N_RECORDS]);
        }
        BUF_LIT(b, "]}");
        RESPOND(w, c, "HTTP/1.1 200 OK\r\n", "application/json", b->p, b->len, head, rq);
    } else if (path_is(rq, "/compute")) {
        long rounds = query_long(rq, "rounds", DEFAULT_ROUNDS);
        if (rounds < 1)
            rounds = 1;
        if (rounds > MAX_ROUNDS)
            rounds = MAX_ROUNDS;
        long seed = query_long(rq, "seed", (long)(w->nreq ^ ((uint64_t)w->id << 40)));
        char sb[32], hex[65];
        uint8_t h[32];
        int sl = snprintf(sb, sizeof sb, "%ld", seed);
        sha256(sb, (size_t)sl, h);
        for (long i = 1; i < rounds; i++)
            sha256_32(h, h);
        for (int i = 0; i < 32; i++) {
            hex[2 * i] = "0123456789abcdef"[h[i] >> 4];
            hex[2 * i + 1] = "0123456789abcdef"[h[i] & 15];
        }
        buf_t *b = &w->body;
        b->len = 0;
        BUF_LIT(b, "{\"rounds\":");
        buf_u64(b, (uint64_t)rounds);
        BUF_LIT(b, ",\"seed\":");
        buf_put(b, sb, (size_t)sl);
        BUF_LIT(b, ",\"sha256\":\"");
        buf_put(b, hex, 64);
        BUF_LIT(b, "\"}");
        RESPOND(w, c, "HTTP/1.1 200 OK\r\n", "application/json", b->p, b->len, head, rq);
    } else {
        RESPOND(w, c, "HTTP/1.1 404 Not Found\r\n", "text/plain", "Not Found", 9, head, rq);
    }
}

/* Parses and answers every complete request in the receive buffer, then
 * writes all responses at once. Returns -1 if the connection was closed. */
static int conn_process(worker_t *w, conn_t *c)
{
    size_t off = 0;
    while (off < c->rlen && !c->closing) {
        req_t rq;
        long used = parse_request(c->rbuf + off, c->rlen - off, &rq);
        if (used == 0)
            break;
        if (used < 0) {
            if (used == -2)
                RESPOND(w, c, "HTTP/1.1 431 Request Header Fields Too Large\r\n", "text/plain",
                        "Request Too Large", 17, 0, NULL);
            else
                RESPOND(w, c, "HTTP/1.1 400 Bad Request\r\n", "text/plain", "Bad Request", 11,
                        0, NULL);
            c->closing = 1;
            off = c->rlen;
            break;
        }
        handle(w, c, &rq);
        if (!rq.keepalive)
            c->closing = 1;
        off += (size_t)used;
    }
    if (off) {
        memmove(c->rbuf, c->rbuf + off, c->rlen - off);
        c->rlen -= (uint32_t)off;
    }
    if (c->out.len > c->woff)
        return conn_flush(w, c);
    if (c->closing) {
        conn_close(w, c);
        return -1;
    }
    return 0;
}

static int conn_readable(worker_t *w, conn_t *c)
{
    ssize_t r;
    do
        r = recv(c->fd, c->rbuf + c->rlen, RBUF_SIZE - c->rlen, 0);
    while (r < 0 && errno == EINTR);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    if (r <= 0) {
        conn_close(w, c);
        return -1;
    }
    c->rlen += (uint32_t)r;
    return conn_process(w, c);
}

static void do_accept(worker_t *w)
{
    for (int k = 0; k < 128; k++) {
        int fd = accept4(w->lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if ((errno == EMFILE || errno == ENFILE) && w->spare_fd >= 0) {
                /* out of descriptors: shed this connection instead of spinning */
                close(w->spare_fd);
                fd = accept(w->lfd, NULL, NULL);
                if (fd >= 0)
                    close(fd);
                w->spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
                if (!g_quiet)
                    fprintf(stderr, "httpd: out of file descriptors, raise 'ulimit -n'\n");
            }
            return;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        conn_t *c = malloc(sizeof *c);
        if (!c) {
            close(fd);
            return;
        }
        c->fd = fd;
        c->rlen = 0;
        c->closing = 0;
        c->out_armed = 0;
        c->out.p = NULL;
        c->out.len = c->out.cap = 0;
        c->woff = 0;
        struct epoll_event ev = {.events = EPOLLIN, .data.ptr = c};
        if (epoll_ctl(w->ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
            close(fd);
            free(c);
        }
    }
}

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    if (w->cpu >= 0)
        bu_pin_self(w->cpu);
    w->ep = epoll_create1(EPOLL_CLOEXEC);
    if (w->ep < 0)
        bu_die("httpd: epoll_create1: %s", strerror(errno));
    w->spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    struct epoll_event lev = {.events = EPOLLIN, .data.ptr = NULL};
    if (epoll_ctl(w->ep, EPOLL_CTL_ADD, w->lfd, &lev) < 0)
        bu_die("httpd: epoll_ctl: %s", strerror(errno));
    struct epoll_event evs[MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(w->ep, evs, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            bu_die("httpd: epoll_wait: %s", strerror(errno));
        }
        update_date(w);
        for (int i = 0; i < n; i++) {
            conn_t *c = evs[i].data.ptr;
            uint32_t e = evs[i].events;
            if (!c) {
                do_accept(w);
                continue;
            }
            if (e & EPOLLERR) {
                conn_close(w, c);
                continue;
            }
            if ((e & EPOLLOUT) && conn_flush(w, c) < 0)
                continue;
            if (!(e & (EPOLLIN | EPOLLHUP)))
                continue;
            if (c->out_armed) { /* reading is paused until the output drains */
                if (e & EPOLLHUP)
                    conn_close(w, c);
                continue;
            }
            conn_readable(w, c);
        }
    }
    return NULL;
}

static int make_listener(const char *addr, int port, int backlog)
{
    struct addrinfo hints, *res;
    char ps[16];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    snprintf(ps, sizeof ps, "%d", port);
    int rc = getaddrinfo(addr, ps, &hints, &res);
    if (rc != 0)
        bu_die("httpd: cannot resolve '%s': %s", addr, gai_strerror(rc));
    int fd = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        bu_die("httpd: socket: %s", strerror(errno));
    int one = 1, zero = 0;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0)
        bu_die("httpd: SO_REUSEPORT: %s", strerror(errno));
    if (res->ai_family == AF_INET6)
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    if (bind(fd, res->ai_addr, res->ai_addrlen) < 0)
        bu_die("httpd: bind %s:%d: %s", addr, port, strerror(errno));
    if (listen(fd, backlog) < 0)
        bu_die("httpd: listen: %s", strerror(errno));
    freeaddrinfo(res);
    return fd;
}

static void usage(void)
{
    printf("Usage: httpd [options]\n"
           "  -p, --port PORT      listen port (default 8080)\n"
           "  -b, --bind ADDR      bind address (default 0.0.0.0, use :: for IPv6)\n"
           "  -t, --threads N      worker threads (default: CPUs in the affinity mask)\n"
           "      --pin            pin worker i to the i-th CPU of the affinity mask\n"
           "      --backlog N      listen backlog per worker (default 4096)\n"
           "  -q, --quiet\n"
           "  -h, --help\n"
           "Endpoints: /plain  /json?items=N  /compute?rounds=N\n");
}

int main(int argc, char **argv)
{
    const char *bind_addr = "0.0.0.0";
    int port = 8080, threads = 0, pin = 0, backlog = 4096;
    static const struct option opts[] = {
        {"port", required_argument, 0, 'p'}, {"bind", required_argument, 0, 'b'},
        {"threads", required_argument, 0, 't'}, {"pin", no_argument, 0, 'P'},
        {"backlog", required_argument, 0, 'B'}, {"quiet", no_argument, 0, 'q'},
        {"help", no_argument, 0, 'h'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "p:b:t:qh", opts, NULL)) != -1) {
        switch (c) {
        case 'p': port = atoi(optarg); break;
        case 'b': bind_addr = optarg; break;
        case 't': threads = atoi(optarg); break;
        case 'P': pin = 1; break;
        case 'B': backlog = atoi(optarg); break;
        case 'q': g_quiet = 1; break;
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }
    if (port <= 0 || port > 65535)
        bu_die("httpd: bad port");
    signal(SIGPIPE, SIG_IGN);
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    if (sha256_selftest() != 0)
        bu_die("httpd: SHA-256 self test failed");
    make_records();

    static int cpus[BU_MAX_CPUS];
    int ncpus = bu_affinity_cpus(cpus, BU_MAX_CPUS);
    bu_cpu_order(cpus, ncpus);
    if (threads <= 0)
        threads = ncpus;
    worker_t *ws = bu_xcalloc((size_t)threads, sizeof *ws);
    for (int i = 0; i < threads; i++) { /* bind all sockets first: fail fast */
        ws[i].id = i;
        ws[i].cpu = pin ? cpus[i % ncpus] : -1;
        ws[i].lfd = make_listener(bind_addr, port, backlog);
        update_date(&ws[i]);
    }
    for (int i = 0; i < threads; i++)
        if (pthread_create(&ws[i].th, NULL, worker_main, &ws[i]) != 0)
            bu_die("httpd: pthread_create failed");
    if (!g_quiet)
        fprintf(stderr, "httpd: listening on %s:%d with %d worker thread%s%s\n", bind_addr, port,
                threads, threads == 1 ? "" : "s", pin ? " (pinned)" : "");
    for (int i = 0; i < threads; i++)
        pthread_join(ws[i].th, NULL);
    return 0;
}
