/* TCP NDJSON server (T018).
 *
 * Sharded poll() event loops: N loop threads, each with its own SO_REUSEPORT
 * listener and its own connection set, doing non-blocking I/O. A connection is
 * owned entirely by one loop thread — its request is dispatched inline — so the
 * number of concurrent connections is no longer bounded by the thread count
 * (an idle connection costs an fd + a small buffer, not a thread). Per
 * connection there is at most one in-flight request: the next request is not
 * read until the current response has fully drained, which preserves response
 * ordering and provides natural backpressure. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aegisdb/tcp_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "aegisdb/json_request.h"
#include "aegisdb/json_response.h"
#include "aegisdb/logging.h"

#define POLL_TIMEOUT_MS 200 /* re-check g_stop + idle-reap at least this often */

/* Set from a signal handler and read by every loop thread; atomic for both
 * (lock-free atomic_int store is async-signal-safe). */
static atomic_int g_stop;

/* Total live client connections across all loop threads, for --max-connections. */
static atomic_int g_conns;

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void tcp_server_request_stop(void) {
    atomic_store_explicit(&g_stop, 1, memory_order_relaxed);
}

static int stop_requested(void) {
    return atomic_load_explicit(&g_stop, memory_order_relaxed);
}

/* Per-connection state. Owned by a single loop thread. */
typedef struct {
    int fd;
    char peer[32];      /* "ip:port" for log context */
    char *rbuf;         /* request accumulation */
    size_t rlen, rcap;
    char *wbuf;         /* pending response bytes [woff, wlen); owned, malloc'd */
    size_t wlen, woff;
    int want_write;     /* a response is staged and not yet fully sent */
    int close_after_write; /* close once wbuf drains (e.g. oversized-line error) */
    unsigned long requests;
    uint64_t last_activity; /* mono_ms() of last byte progress; for idle reaping */
} Conn;

/* Maximum accepted request line: payload limit plus JSON envelope slack. */
static size_t max_line(const AegisDB *db) {
    return db->config.max_payload_bytes + 4096;
}

static void conn_free(Conn *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c->rbuf);
    free(c->wbuf);
    free(c);
    atomic_fetch_sub_explicit(&g_conns, 1, memory_order_relaxed);
}

/* Flush staged response bytes. Returns 0 to keep the connection, -1 to close
 * (write error, or a fully-drained response that requested close). */
static int conn_flush(Conn *c) {
    while (c->woff < c->wlen) {
        ssize_t w = write(c->fd, c->wbuf + c->woff, c->wlen - c->woff);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0; /* wait POLLOUT */
            return -1;
        }
        c->woff += (size_t)w;
        c->last_activity = mono_ms(); /* progress: not idle */
    }
    free(c->wbuf);
    c->wbuf = NULL;
    c->wlen = c->woff = 0;
    c->want_write = 0;
    return c->close_after_write ? -1 : 0;
}

/* Process complete request lines buffered in rbuf, dispatching each inline and
 * staging its response. Stops at the first incomplete line, or once a response
 * is staged but not fully sent (one in-flight request at a time). Returns 0 to
 * keep the connection, -1 to close. */
static int conn_advance(Conn *c, AegisDB *db, size_t limit) {
    while (!c->want_write) {
        char *nl = c->rlen ? memchr(c->rbuf, '\n', c->rlen) : NULL;
        if (!nl) {
            if (c->rlen > limit) {
                LOG_WARN("connection from %s: request line exceeds limit "
                         "(%zu > %zu bytes); rejecting",
                         c->peer, c->rlen, limit);
                size_t rl = 0;
                char *resp = json_finish_line(
                    json_error_status(AEGIS_ERR_PAYLOAD_TOO_LARGE), NULL, &rl);
                if (!resp) return -1;
                c->wbuf = resp;
                c->wlen = rl;
                c->woff = 0;
                c->want_write = 1;
                c->close_after_write = 1;
                return conn_flush(c);
            }
            return 0; /* need more bytes */
        }
        size_t line_len = (size_t)(nl - c->rbuf);
        size_t eff = line_len;
        if (eff > 0 && c->rbuf[eff - 1] == '\r') eff--; /* tolerate CRLF */

        char *resp = NULL;
        size_t rl = 0;
        if (eff > 0) resp = aegis_request_handle(db, c->rbuf, eff, &rl);

        /* consume the line (and its newline) from the read buffer */
        size_t consumed = line_len + 1;
        memmove(c->rbuf, c->rbuf + consumed, c->rlen - consumed);
        c->rlen -= consumed;

        if (resp) {
            c->requests++;
            LOG_DEBUG("connection from %s: handled request #%lu (%zu in, %zu out)",
                      c->peer, c->requests, eff, rl);
            c->wbuf = resp;
            c->wlen = rl;
            c->woff = 0;
            c->want_write = 1;
            if (conn_flush(c) == -1) return -1;
        }
    }
    return 0;
}

/* Drain readable bytes into rbuf. Returns 0 (ok, wait for more), 1 (peer closed
 * cleanly — caller should still process buffered lines), or -1 (error). */
static int conn_read(Conn *c, size_t limit) {
    for (;;) {
        if (c->rlen == c->rcap) {
            if (c->rcap > limit) return 0; /* full w/o newline: advance() rejects */
            size_t ncap = c->rcap ? c->rcap * 2 : 4096;
            if (ncap > limit + 1) ncap = limit + 1;
            char *nb = realloc(c->rbuf, ncap);
            if (!nb) return -1;
            c->rbuf = nb;
            c->rcap = ncap;
        }
        ssize_t r = read(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen);
        if (r == 0) return 1; /* EOF */
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        c->rlen += (size_t)r;
        c->last_activity = mono_ms(); /* progress: not idle */
    }
}

/* Accept all pending connections on the listener into the loop's conn set.
 * `maxconn` (0 = unlimited) caps total concurrent connections across all loops. */
static void loop_accept(int lfd, Conn ***conns, size_t *n, size_t *cap,
                        int maxconn) {
    for (;;) {
        struct sockaddr_in paddr;
        socklen_t plen = sizeof(paddr);
        int cfd = accept4(lfd, (struct sockaddr *)&paddr, &plen, SOCK_NONBLOCK);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break; /* EAGAIN/EWOULDBLOCK: drained, or a transient accept error */
        }
        /* Refuse past the hard cap: an accepted-then-immediately-closed socket
         * is a cheap, bounded response to a flood. */
        if (maxconn > 0 &&
            atomic_load_explicit(&g_conns, memory_order_relaxed) >= maxconn) {
            close(cfd);
            continue;
        }
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        Conn *c = calloc(1, sizeof(*c));
        if (!c) {
            close(cfd);
            continue;
        }
        atomic_fetch_add_explicit(&g_conns, 1, memory_order_relaxed);
        c->fd = cfd;
        c->last_activity = mono_ms();
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &paddr.sin_addr, ip, sizeof(ip)))
            snprintf(c->peer, sizeof(c->peer), "%s:%u", ip,
                     (unsigned)ntohs(paddr.sin_port));
        else
            snprintf(c->peer, sizeof(c->peer), "?");
        if (*n == *cap) {
            size_t nc = *cap ? *cap * 2 : 16;
            Conn **g = realloc(*conns, nc * sizeof(**conns));
            if (!g) {
                conn_free(c);
                continue;
            }
            *conns = g;
            *cap = nc;
        }
        (*conns)[(*n)++] = c;
        LOG_DEBUG("connection accepted from %s (fd %d)", c->peer, cfd);
    }
}

typedef struct {
    AegisDB *db;
    int lfd;
} Loop;

static void *loop_main(void *arg) {
    Loop *L = arg;
    AegisDB *db = L->db;
    int lfd = L->lfd;
    const size_t limit = max_line(db);
    const int maxconn = db->config.max_connections;
    const uint64_t idle_ms = (uint64_t)db->config.idle_timeout_sec * 1000ULL;

    Conn **conns = NULL;
    size_t nconns = 0, ccap = 0;
    struct pollfd *pfds = NULL;
    size_t pcap = 0;

    while (!stop_requested()) {
        size_t need = nconns + 1; /* +1 for the listener */
        if (need > pcap) {
            struct pollfd *np = realloc(pfds, need * sizeof(*np));
            if (!np) break;
            pfds = np;
            pcap = need;
        }
        pfds[0].fd = lfd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        for (size_t i = 0; i < nconns; i++) {
            pfds[i + 1].fd = conns[i]->fd;
            pfds[i + 1].events = conns[i]->want_write ? POLLOUT : POLLIN;
            pfds[i + 1].revents = 0;
        }

        int pr = poll(pfds, need, POLL_TIMEOUT_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* pr == 0 is a timeout: no I/O to service, but still fall through to the
         * idle-reap pass below so slow-loris / stalled sockets are collected. */
        if (pr > 0) {
            /* Only these connections were in the poll set. loop_accept may append
             * more below; those have no revents yet and are processed next poll. */
            size_t polled = nconns;

            if (pfds[0].revents & POLLIN)
                loop_accept(lfd, &conns, &nconns, &ccap, maxconn);

            for (size_t i = 0; i < polled; i++) {
                Conn *c = conns[i];
                short re = pfds[i + 1].revents;
                if (re == 0) continue;
                int closeit = 0;
                if (re & POLLIN) {
                    int rr = conn_read(c, limit);
                    if (rr < 0) closeit = 1;
                    else if (conn_advance(c, db, limit) == -1) closeit = 1;
                    else if (rr == 1) closeit = 1; /* EOF: buffered lines processed */
                }
                if (!closeit && (re & POLLOUT)) {
                    if (conn_flush(c) == -1) closeit = 1;
                    else if (conn_advance(c, db, limit) == -1) closeit = 1;
                }
                if (re & (POLLERR | POLLHUP | POLLNVAL)) closeit = 1;
                if (closeit) {
                    LOG_DEBUG("connection from %s closed after %lu request(s)",
                              c->peer, c->requests);
                    conn_free(c);
                    conns[i] = NULL;
                }
            }
        }

        /* Reap connections that have moved no bytes for idle_ms (slow-loris,
         * abandoned sockets). last_activity is stamped on accept and on every
         * read/write that makes progress, so an active peer is never reaped. */
        if (idle_ms) {
            uint64_t now = mono_ms();
            for (size_t i = 0; i < nconns; i++) {
                Conn *c = conns[i];
                if (!c) continue;
                if (now - c->last_activity > idle_ms) {
                    LOG_DEBUG("connection from %s reaped: idle %llu ms", c->peer,
                              (unsigned long long)(now - c->last_activity));
                    conn_free(c);
                    conns[i] = NULL;
                }
            }
        }

        /* compact out closed connections */
        size_t w = 0;
        for (size_t i = 0; i < nconns; i++)
            if (conns[i]) conns[w++] = conns[i];
        nconns = w;
    }

    for (size_t i = 0; i < nconns; i++) conn_free(conns[i]);
    free(conns);
    free(pfds);
    close(lfd);
    return NULL;
}

/* Create a non-blocking SO_REUSEPORT listener bound to `port`. -1 on failure. */
static int make_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    /* Every loop thread binds the same port; the kernel load-balances accepts. */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) != 0) {
        LOG_ERROR("SO_REUSEPORT not available: %s", strerror(errno));
        close(fd);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 128) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int tcp_server_run(AegisDB *db) {
    signal(SIGPIPE, SIG_IGN);

    int nthreads = db->config.io_threads;
    if (nthreads < 1) nthreads = 1;

    /* Create all listeners up front so a bind failure is caught before any
     * thread is spawned. */
    int *lfds = calloc((size_t)nthreads, sizeof(int));
    Loop *loops = calloc((size_t)nthreads, sizeof(Loop));
    pthread_t *threads = calloc((size_t)nthreads, sizeof(pthread_t));
    if (!lfds || !loops || !threads) {
        free(lfds);
        free(loops);
        free(threads);
        return -1;
    }
    for (int i = 0; i < nthreads; i++) lfds[i] = -1;

    int made = 0;
    for (int i = 0; i < nthreads; i++) {
        lfds[i] = make_listener(db->config.listen_port);
        if (lfds[i] < 0) {
            LOG_ERROR("bind to port %d failed: %s", db->config.listen_port,
                      strerror(errno));
            break;
        }
        made++;
    }
    if (made != nthreads) {
        for (int i = 0; i < made; i++) close(lfds[i]);
        free(lfds);
        free(loops);
        free(threads);
        return -1;
    }

    LOG_INFO("listening on 0.0.0.0:%d", db->config.listen_port);
    LOG_INFO("data directory: %s", db->config.data_dir);
    LOG_DEBUG("io threads: %d, max request line: %zu bytes", nthreads,
              max_line(db));

    int spawned = 0;
    for (int i = 0; i < nthreads; i++) {
        loops[i].db = db;
        loops[i].lfd = lfds[i];
        if (pthread_create(&threads[i], NULL, loop_main, &loops[i]) != 0) {
            LOG_ERROR("failed to spawn io thread %d", i);
            break;
        }
        spawned++;
    }
    if (spawned == 0) {
        for (int i = 0; i < nthreads; i++) close(lfds[i]);
        free(lfds);
        free(loops);
        free(threads);
        return -1;
    }
    if (spawned != nthreads) {
        /* a thread failed to start: ask the rest to stop, then close the
         * listeners the unspawned loops would have owned */
        atomic_store_explicit(&g_stop, 1, memory_order_relaxed);
        for (int i = spawned; i < nthreads; i++) close(lfds[i]);
    }

    for (int i = 0; i < spawned; i++) pthread_join(threads[i], NULL);

    LOG_INFO("shutting down");
    free(lfds);
    free(loops);
    free(threads);
    return 0;
}