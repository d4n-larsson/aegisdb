/* Blocking TCP client-side socket helpers (see aegisdb/netio.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aegisdb/netio.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* connect(2) bounded to NET_CONNECT_TIMEOUT_MS so a dead/unreachable peer can't
 * block the caller for the kernel's ~2-minute default — critical for the replica
 * follower, whose shutdown otherwise waits out a connect to an unreachable
 * primary. Returns 0 on success (fd left blocking), -1 on error/timeout. */
#define NET_CONNECT_TIMEOUT_MS 5000
static int connect_timeout(int fd, const struct sockaddr *addr,
                           socklen_t alen) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return -1;
    int rv = -1;
    if (connect(fd, addr, alen) == 0) {
        rv = 0; /* completed immediately (e.g. loopback) */
    } else if (errno == EINPROGRESS) {
        uint64_t deadline = net_mono_ms() + NET_CONNECT_TIMEOUT_MS;
        for (;;) {
            uint64_t now = net_mono_ms();
            struct pollfd p = {.fd = fd, .events = POLLOUT};
            int pr = poll(&p, 1, now >= deadline ? 0 : (int)(deadline - now));
            if (pr > 0) {
                int err = 0;
                socklen_t elen = sizeof err;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 &&
                    err == 0)
                    rv = 0;
                break;
            }
            if (pr == 0)
                break; /* timed out */
            if (errno == EINTR)
                continue; /* signal: retry within the deadline */
            break;        /* poll error */
        }
    }
    if (rv == 0)
        fcntl(fd, F_SETFL, fl); /* restore blocking; caller closes on -1 */
    return rv;
}

int net_dial(const char *host, const char *port) {
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0)
        return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype | SOCK_CLOEXEC,
                    rp->ai_protocol);
        if (fd < 0)
            continue;
        if (connect_timeout(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

void net_set_timeouts(int fd, int secs) {
    struct timeval tv = {.tv_sec = secs, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

int net_write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int net_write_str(int fd, const char *s) {
    return net_write_all(fd, s, strlen(s));
}

int net_read_full(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1; /* EOF */
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

uint64_t net_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

int net_read_line(int fd, char *buf, size_t cap, uint64_t deadline_ms) {
    size_t i = 0;
    while (i + 1 < cap) {
        if (deadline_ms && net_mono_ms() >= deadline_ms)
            return -1;
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        if (c == '\n')
            break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return (int)i;
}