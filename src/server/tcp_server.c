/* TCP NDJSON server: accept, read lines, dispatch, write responses (T018). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aegisdb/tcp_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aegisdb/json_request.h"
#include "aegisdb/json_response.h"
#include "aegisdb/logging.h"
#include "aegisdb/thread_pool.h"

static volatile sig_atomic_t g_stop = 0;

void tcp_server_request_stop(void) { g_stop = 1; }

typedef struct {
    int fd;
    AegisDB *db;
    char peer[32]; /* "ip:port" for log context */
} Conn;

static int write_all(int fd, const char *buf, size_t n) {
    size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, buf + put, n - put);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        put += (size_t)w;
    }
    return 0;
}

/* Maximum accepted request line: payload limit plus JSON envelope slack. */
static size_t max_line(const AegisDB *db) {
    return db->config.max_payload_bytes + 4096;
}

static void handle_connection(void *arg) {
    Conn *c = arg;
    int fd = c->fd;
    AegisDB *db = c->db;
    char peer[32];
    snprintf(peer, sizeof(peer), "%s", c->peer);
    free(c);

    LOG_DEBUG("connection accepted from %s (fd %d)", peer, fd);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        LOG_ERROR("connection from %s: out of memory allocating read buffer",
                  peer);
        close(fd);
        return;
    }
    const size_t limit = max_line(db);
    unsigned long requests = 0;

    for (;;) {
        char *nl = memchr(buf, '\n', len);
        if (!nl) {
            if (len > limit) {
                LOG_WARN("connection from %s: request line exceeds limit "
                         "(%zu > %zu bytes); rejecting",
                         peer, len, limit);
                size_t rl = 0;
                char *resp = json_finish_line(
                    json_error_status(AEGIS_ERR_PAYLOAD_TOO_LARGE), NULL, &rl);
                if (resp) {
                    write_all(fd, resp, rl);
                    free(resp);
                }
                break;
            }
            if (len == cap) {
                size_t ncap = cap * 2;
                char *nb = realloc(buf, ncap);
                if (!nb) break;
                buf = nb;
                cap = ncap;
            }
            ssize_t r = read(fd, buf + len, cap - len);
            if (r == 0) break; /* peer closed */
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }
            len += (size_t)r;
            continue;
        }
        size_t line_len = (size_t)(nl - buf);
        /* trim trailing CR for CRLF clients */
        size_t eff = line_len;
        if (eff > 0 && buf[eff - 1] == '\r') eff--;
        if (eff > 0) {
            size_t rl = 0;
            char *resp = aegis_request_handle(db, buf, eff, &rl);
            requests++;
            LOG_DEBUG("connection from %s: handled request #%lu (%zu bytes in, "
                      "%zu bytes out)",
                      peer, requests, eff, resp ? rl : (size_t)0);
            if (resp) {
                int rv = write_all(fd, resp, rl);
                free(resp);
                if (rv != 0) {
                    LOG_DEBUG("connection from %s: write failed, closing", peer);
                    break;
                }
            }
        }
        /* shift remaining bytes after the newline to the front */
        size_t rest = len - (line_len + 1);
        memmove(buf, nl + 1, rest);
        len = rest;
    }

    free(buf);
    close(fd);
    LOG_DEBUG("connection from %s closed after %lu request(s)", peer, requests);
}

int tcp_server_run(AegisDB *db) {
    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        LOG_ERROR("socket() failed: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)db->config.listen_port);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_ERROR("bind to port %d failed: %s", db->config.listen_port,
                  strerror(errno));
        close(lfd);
        return -1;
    }
    if (listen(lfd, 128) != 0) {
        LOG_ERROR("listen() failed: %s", strerror(errno));
        close(lfd);
        return -1;
    }

    ThreadPool *tp = thread_pool_create(db->config.worker_threads);
    if (!tp) {
        LOG_ERROR("failed to create worker thread pool (%d threads)",
                  db->config.worker_threads);
        close(lfd);
        return -1;
    }

    LOG_INFO("listening on 0.0.0.0:%d", db->config.listen_port);
    LOG_INFO("data directory: %s", db->config.data_dir);
    LOG_DEBUG("worker threads: %d, max request line: %zu bytes",
              db->config.worker_threads, max_line(db));

    while (!g_stop) {
        struct pollfd pfd = {.fd = lfd, .events = POLLIN};
        int pr = poll(&pfd, 1, 500);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue; /* timeout: re-check g_stop */
        struct sockaddr_in paddr;
        socklen_t plen = sizeof(paddr);
        int cfd = accept(lfd, (struct sockaddr *)&paddr, &plen);
        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            LOG_ERROR("accept() failed: %s", strerror(errno));
            break;
        }
        Conn *c = malloc(sizeof(*c));
        if (!c) {
            LOG_ERROR("out of memory accepting connection; dropping it");
            close(cfd);
            continue;
        }
        c->fd = cfd;
        c->db = db;
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &paddr.sin_addr, ip, sizeof(ip)))
            snprintf(c->peer, sizeof(c->peer), "%s:%u", ip,
                     (unsigned)ntohs(paddr.sin_port));
        else
            snprintf(c->peer, sizeof(c->peer), "?");
        if (thread_pool_submit(tp, handle_connection, c) != 0) {
            LOG_WARN("worker queue full; dropping connection from %s", c->peer);
            close(cfd);
            free(c);
        }
    }

    LOG_INFO("shutting down");
    close(lfd);
    thread_pool_destroy(tp);
    return 0;
}