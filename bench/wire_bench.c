/* End-to-end wire-protocol benchmark (#load).
 *
 * Drives a RUNNING aegisdb server over TCP with the real NDJSON protocol and
 * reports throughput and latency percentiles for the full request pipeline
 * (socket -> framing -> cJSON parse -> query router -> rwlock -> storage).
 * Unlike hnsw_bench (which links the index in isolation), this measures the
 * product as clients actually experience it.
 *
 * Start a server first, then point this at it, e.g.:
 *
 *   ./build/aegisdb --data-dir /tmp/bench --port 9470 --embedding-dim 384 &
 *   ./build/bench/wire_bench --port 9470 --op mixed --conns 16 --secs 10
 *
 * Options (all optional; sensible defaults):
 *   --host H        server host (default 127.0.0.1)
 *   --port N        server port (default 9470)
 *   --conns N       concurrent connections/threads (default 8)
 *   --secs N        timed run duration in seconds (default 5)
 *   --op WL         workload: ping|insert|get|search-vec|search-tag|mixed
 *                   (default mixed: ~20% insert, ~80% get)
 *   --dim N         embedding dimension; must match the server (default 384)
 *   --preload N     records to insert before the timed run so get/search hit
 *                   real ids (default 5000; forced >0 for get/search/mixed)
 *   --payload N     insert payload size in bytes (default 64)
 *   --top-k N       search top_k (default 10)
 *   --token T       auth token sent on every request (default none)
 *   --seed N        RNG seed (default 1)
 *   --no-embeddings send include_embeddings:false so responses omit vectors
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum workload { WL_PING, WL_INSERT, WL_GET, WL_SEARCH_VEC, WL_SEARCH_TAG, WL_MIXED };

struct config {
    const char *host;
    int port;
    int conns;
    double secs;
    enum workload wl;
    size_t dim;
    size_t preload;
    size_t payload;
    int top_k;
    const char *token;
    uint64_t seed;
    int no_embeddings; /* send include_embeddings:false on get/search */
};

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* xorshift so runs are deterministic and cheap (no libc rand lock contention) */
static uint64_t xrand(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

static int connect_server(const char *host, int port) {
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        /* small req/resp: without NODELAY, Nagle+delayed-ACK adds ~40ms and we
         * would measure the interaction, not the server */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

/* Write the whole buffer. */
static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* Read one NDJSON line (through the terminating '\n') into buf. Returns line
 * length (excluding NUL) or -1 on error/EOF. Leftover bytes after the newline
 * stay buffered in the reader for the next call (pipelining-safe). */
/* Large enough for a full search response: top_k records each carrying a
 * float embedding (~8 KB/record at dim=384) plus payload and metadata. */
#define READER_CAP (1 << 21) /* 2 MiB */
struct reader {
    char buf[READER_CAP];
    size_t len; /* buffered bytes not yet consumed */
};

static int read_line(int fd, struct reader *r, char *out, size_t out_cap) {
    size_t consumed = 0;
    for (;;) {
        /* scan buffered bytes for a newline */
        for (size_t i = 0; i < r->len; i++) {
            if (r->buf[i] == '\n') {
                size_t linelen = i;
                if (linelen >= out_cap) linelen = out_cap - 1;
                memcpy(out, r->buf, linelen);
                out[linelen] = '\0';
                /* shift the remainder down */
                memmove(r->buf, r->buf + i + 1, r->len - i - 1);
                r->len -= i + 1;
                return (int)linelen;
            }
        }
        consumed = r->len;
        if (consumed >= sizeof r->buf) return -1; /* line too long */
        ssize_t n = read(fd, r->buf + r->len, sizeof r->buf - r->len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; /* EOF */
        r->len += (size_t)n;
    }
}

/* Parse the first unsigned integer value of "id":<n> from a response line. */
static long parse_id(const char *s) {
    const char *p = strstr(s, "\"id\":");
    if (!p) return -1;
    p += 5;
    while (*p == ' ') p++;
    return strtol(p, NULL, 10);
}

/* Build a comma-separated embedding literal into buf. */
static size_t build_embedding(char *buf, size_t cap, size_t dim, uint64_t *rng) {
    size_t off = 0;
    for (size_t d = 0; d < dim; d++) {
        float v = (float)((xrand(rng) >> 11) * (1.0 / 9007199254740992.0)) * 2.0f - 1.0f;
        int n = snprintf(buf + off, cap - off, "%s%.4f", d ? "," : "", v);
        if (n < 0 || (size_t)n >= cap - off) break;
        off += (size_t)n;
    }
    return off;
}

/* ------- shared state ---------------------------------------------------- */

static struct config g_cfg;
static long *g_ids;         /* ids captured during preload */
static size_t g_id_count;   /* how many valid ids in g_ids */
static volatile int g_stop; /* set when the timed run's deadline passes */
static pthread_barrier_t g_barrier;

struct result {
    double *lat;    /* per-op latency samples (ms) */
    size_t n, cap;
    long errors;
};

static void rec(struct result *r, double ms) {
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 4096;
        r->lat = realloc(r->lat, r->cap * sizeof *r->lat);
    }
    r->lat[r->n++] = ms;
}

/* One request/response round-trip; returns 0 on ok, -1 on transport error,
 * 1 on an ok:false response. */
static int roundtrip(int fd, struct reader *rd, const char *req, size_t reqlen,
                     char *resp, size_t resp_cap, long *out_id) {
    if (write_all(fd, req, reqlen) != 0) return -1;
    int n = read_line(fd, rd, resp, resp_cap);
    if (n < 0) return -1;
    if (out_id) *out_id = parse_id(resp);
    /* "\"ok\":true" present == success */
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

struct thread_arg {
    int idx;
    struct result res;
};

/* Build one workload request into `req`; returns its length. `needs_id` is
 * filled with the id we want back (get), or -1. */
static size_t build_request(enum workload wl, char *req, size_t cap, char *emb,
                            uint64_t *rng, const char *tokfield) {
    const struct config *c = &g_cfg;
    switch (wl) {
    case WL_PING:
        return (size_t)snprintf(req, cap, "{\"operation\":\"ping\"}\n");
    case WL_GET: {
        long id = g_id_count ? g_ids[xrand(rng) % g_id_count] : 1;
        return (size_t)snprintf(req, cap,
                                "{\"operation\":\"get\",\"id\":%ld%s}\n", id,
                                tokfield);
    }
    case WL_SEARCH_TAG:
        return (size_t)snprintf(
            req, cap,
            "{\"operation\":\"search\",\"tags\":[\"bench\"],\"top_k\":%d%s}\n",
            c->top_k, tokfield);
    case WL_SEARCH_VEC: {
        build_embedding(emb, 1 << 16, c->dim, rng);
        return (size_t)snprintf(req, cap,
                                "{\"operation\":\"search\",\"embedding\":[%s],"
                                "\"top_k\":%d%s}\n",
                                emb, c->top_k, tokfield);
    }
    case WL_INSERT:
    default: {
        build_embedding(emb, 1 << 16, c->dim, rng);
        /* fixed-size payload of 'x' */
        static __thread char pay[65536];
        size_t plen = c->payload < sizeof pay - 1 ? c->payload : sizeof pay - 1;
        memset(pay, 'x', plen);
        pay[plen] = '\0';
        return (size_t)snprintf(
            req, cap,
            "{\"operation\":\"insert\",\"type\":\"semantic\",\"tags\":[\"bench\"],"
            "\"data\":\"%s\",\"embedding\":[%s]%s}\n",
            pay, emb, tokfield);
    }
    }
}

static void *worker(void *v) {
    struct thread_arg *ta = v;
    uint64_t rng = g_cfg.seed + (uint64_t)ta->idx * 0x9E3779B97F4A7C15ULL + 1;
    int fd = connect_server(g_cfg.host, g_cfg.port);
    if (fd < 0) {
        fprintf(stderr, "conn %d: connect failed\n", ta->idx);
        pthread_barrier_wait(&g_barrier);
        return NULL;
    }
    struct reader rd = {0};
    char *req = malloc(1 << 17);
    char *emb = malloc(1 << 16);
    char *resp = malloc(READER_CAP);
    char tokfield[256] = "";
    size_t tf = 0;
    if (g_cfg.token)
        tf += (size_t)snprintf(tokfield + tf, sizeof tokfield - tf,
                               ",\"token\":\"%s\"", g_cfg.token);
    if (g_cfg.no_embeddings)
        snprintf(tokfield + tf, sizeof tokfield - tf,
                 ",\"include_embeddings\":false");

    pthread_barrier_wait(&g_barrier); /* all threads start the timed run together */

    while (!g_stop) {
        enum workload wl = g_cfg.wl;
        if (wl == WL_MIXED) wl = (xrand(&rng) % 5 == 0) ? WL_INSERT : WL_GET;
        size_t len = build_request(wl, req, 1 << 17, emb, &rng, tokfield);
        double t0 = now_s();
        int rc = roundtrip(fd, &rd, req, len, resp, READER_CAP, NULL);
        double ms = (now_s() - t0) * 1e3;
        if (rc < 0) {
            ta->res.errors++;
            break; /* transport dead */
        }
        if (rc > 0) ta->res.errors++;
        rec(&ta->res, ms);
    }
    free(req);
    free(emb);
    free(resp);
    close(fd);
    return NULL;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Insert `n` records on one connection, capturing returned ids. */
static size_t preload(size_t n) {
    int fd = connect_server(g_cfg.host, g_cfg.port);
    if (fd < 0) return 0;
    struct reader rd = {0};
    char *req = malloc(1 << 17);
    char *emb = malloc(1 << 16);
    char *resp = malloc(READER_CAP);
    char tokfield[256] = "";
    size_t tf = 0;
    if (g_cfg.token)
        tf += (size_t)snprintf(tokfield + tf, sizeof tokfield - tf,
                               ",\"token\":\"%s\"", g_cfg.token);
    if (g_cfg.no_embeddings)
        snprintf(tokfield + tf, sizeof tokfield - tf,
                 ",\"include_embeddings\":false");
    uint64_t rng = g_cfg.seed ^ 0xDEADBEEF;
    size_t got = 0;
    for (size_t i = 0; i < n; i++) {
        size_t len = build_request(WL_INSERT, req, 1 << 17, emb, &rng, tokfield);
        long id = -1;
        if (roundtrip(fd, &rd, req, len, resp, READER_CAP, &id) != 0) break;
        if (id >= 0) g_ids[got++] = id;
    }
    free(req);
    free(emb);
    free(resp);
    close(fd);
    return got;
}

static enum workload parse_wl(const char *s) {
    if (!strcmp(s, "ping")) return WL_PING;
    if (!strcmp(s, "insert")) return WL_INSERT;
    if (!strcmp(s, "get")) return WL_GET;
    if (!strcmp(s, "search-vec")) return WL_SEARCH_VEC;
    if (!strcmp(s, "search-tag")) return WL_SEARCH_TAG;
    if (!strcmp(s, "mixed")) return WL_MIXED;
    fprintf(stderr, "unknown --op '%s'\n", s);
    exit(2);
}

int main(int argc, char **argv) {
    g_cfg = (struct config){.host = "127.0.0.1",
                            .port = 9470,
                            .conns = 8,
                            .secs = 5,
                            .wl = WL_MIXED,
                            .dim = 384,
                            .preload = 5000,
                            .payload = 64,
                            .top_k = 10,
                            .token = NULL,
                            .seed = 1};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
#define NEXT() (v ? (i++, v) : (fprintf(stderr, "%s needs a value\n", a), exit(2), ""))
        if (!strcmp(a, "--host")) g_cfg.host = NEXT();
        else if (!strcmp(a, "--port")) g_cfg.port = atoi(NEXT());
        else if (!strcmp(a, "--conns")) g_cfg.conns = atoi(NEXT());
        else if (!strcmp(a, "--secs")) g_cfg.secs = atof(NEXT());
        else if (!strcmp(a, "--op")) g_cfg.wl = parse_wl(NEXT());
        else if (!strcmp(a, "--dim")) g_cfg.dim = strtoul(NEXT(), NULL, 10);
        else if (!strcmp(a, "--preload")) g_cfg.preload = strtoul(NEXT(), NULL, 10);
        else if (!strcmp(a, "--payload")) g_cfg.payload = strtoul(NEXT(), NULL, 10);
        else if (!strcmp(a, "--top-k")) g_cfg.top_k = atoi(NEXT());
        else if (!strcmp(a, "--token")) g_cfg.token = NEXT();
        else if (!strcmp(a, "--seed")) g_cfg.seed = strtoull(NEXT(), NULL, 10);
        else if (!strcmp(a, "--no-embeddings")) g_cfg.no_embeddings = 1;
        else { fprintf(stderr, "unknown arg '%s'\n", a); return 2; }
#undef NEXT
    }
    if (g_cfg.conns < 1) g_cfg.conns = 1;

    /* get/search/mixed need real ids in the DB */
    int needs_data = g_cfg.wl == WL_GET || g_cfg.wl == WL_MIXED ||
                     g_cfg.wl == WL_SEARCH_VEC || g_cfg.wl == WL_SEARCH_TAG;
    if (needs_data && g_cfg.preload == 0) g_cfg.preload = 5000;

    printf("wire_bench: host=%s port=%d conns=%d secs=%.1f op=%d dim=%zu "
           "preload=%zu payload=%zu top_k=%d\n",
           g_cfg.host, g_cfg.port, g_cfg.conns, g_cfg.secs, g_cfg.wl, g_cfg.dim,
           g_cfg.preload, g_cfg.payload, g_cfg.top_k);

    if (g_cfg.preload) {
        g_ids = malloc(g_cfg.preload * sizeof *g_ids);
        double t0 = now_s();
        g_id_count = preload(g_cfg.preload);
        double el = now_s() - t0;
        printf("preload: %zu records in %.2fs (%.0f inserts/s)\n", g_id_count, el,
               g_id_count / el);
        if (needs_data && g_id_count == 0) {
            fprintf(stderr, "preload produced no ids; aborting\n");
            return 1;
        }
    }

    pthread_barrier_init(&g_barrier, NULL, (unsigned)g_cfg.conns + 1);
    struct thread_arg *args = calloc((size_t)g_cfg.conns, sizeof *args);
    pthread_t *tids = calloc((size_t)g_cfg.conns, sizeof *tids);
    for (int i = 0; i < g_cfg.conns; i++) {
        args[i].idx = i;
        pthread_create(&tids[i], NULL, worker, &args[i]);
    }

    pthread_barrier_wait(&g_barrier); /* release workers */
    double start = now_s();
    /* poll the clock rather than sleep() so the reported window matches the
     * workers' own stop check */
    while (now_s() - start < g_cfg.secs) usleep(2000);
    g_stop = 1;
    double elapsed = now_s() - start;

    for (int i = 0; i < g_cfg.conns; i++) pthread_join(tids[i], NULL);

    /* merge all latency samples */
    size_t total = 0;
    long errors = 0;
    for (int i = 0; i < g_cfg.conns; i++) {
        total += args[i].res.n;
        errors += args[i].res.errors;
    }
    double *all = malloc((total ? total : 1) * sizeof *all);
    size_t off = 0;
    for (int i = 0; i < g_cfg.conns; i++) {
        memcpy(all + off, args[i].res.lat, args[i].res.n * sizeof *all);
        off += args[i].res.n;
        free(args[i].res.lat);
    }
    qsort(all, total, sizeof *all, cmp_double);

#define PCT(p) (total ? all[(size_t)((p) / 100.0 * (double)(total - 1))] : 0.0)
    printf("\n--- results ---\n");
    printf("ops:          %zu  (%ld errors)\n", total, errors);
    printf("elapsed:      %.2f s\n", elapsed);
    printf("throughput:   %.0f ops/s\n", total / elapsed);
    printf("latency ms:   p50=%.3f  p90=%.3f  p99=%.3f  p99.9=%.3f  max=%.3f\n",
           PCT(50), PCT(90), PCT(99), PCT(99.9), total ? all[total - 1] : 0.0);
#undef PCT

    free(all);
    free(args);
    free(tids);
    free(g_ids);
    pthread_barrier_destroy(&g_barrier);
    return 0;
}