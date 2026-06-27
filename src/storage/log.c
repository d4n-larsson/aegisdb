/* Append-only log: CRC32 + length framed records (T011). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aegisdb/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/crc32.h"

static int read_full(int fd, off_t off, void *buf, size_t n) {
    uint8_t *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, p + got, n - got, off + (off_t)got);
        if (r == 0) return 1;       /* EOF before n bytes */
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static int write_full(int fd, off_t off, const void *buf, size_t n) {
    const uint8_t *p = buf;
    size_t put = 0;
    while (put < n) {
        ssize_t w = pwrite(fd, p + put, n - put, off + (off_t)put);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        put += (size_t)w;
    }
    return 0;
}

static void put_u32le(uint8_t *b, uint32_t v) {
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get_u32le(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

int log_open(LogFile *lf, const char *path, size_t fsync_batch) {
    memset(lf, 0, sizeof(*lf));
    strncpy(lf->path, path, sizeof(lf->path) - 1);
    lf->fsync_batch = fsync_batch;
    if (pthread_mutex_init(&lf->wlock, NULL) != 0) return -1;
    lf->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (lf->fd < 0) {
        pthread_mutex_destroy(&lf->wlock);
        return -1;
    }
    off_t end = lseek(lf->fd, 0, SEEK_END);
    if (end < 0) {
        close(lf->fd);
        pthread_mutex_destroy(&lf->wlock);
        return -1;
    }
    lf->size = end;
    return 0;
}

void log_close(LogFile *lf) {
    if (lf->fd >= 0) {
        fsync(lf->fd);
        close(lf->fd);
        lf->fd = -1;
    }
    pthread_mutex_destroy(&lf->wlock);
}

void log_fsync(LogFile *lf) {
    if (lf->fd >= 0) fsync(lf->fd);
}

int log_append(LogFile *lf, const uint8_t *payload, size_t len,
               uint64_t *out_offset) {
    if (len > 0xFFFFFFFFu) return -1;
    uint8_t hdr[LOG_FRAME_HEADER];
    uint32_t crc = crc32_compute(payload, len);
    put_u32le(hdr, crc);
    put_u32le(hdr + 4, (uint32_t)len);

    pthread_mutex_lock(&lf->wlock);
    off_t off = lf->size;
    if (write_full(lf->fd, off, hdr, LOG_FRAME_HEADER) != 0 ||
        write_full(lf->fd, off + LOG_FRAME_HEADER, payload, len) != 0) {
        pthread_mutex_unlock(&lf->wlock);
        return -1;
    }
    lf->size = off + LOG_FRAME_HEADER + (off_t)len;
    lf->since_fsync++;
    if (lf->fsync_batch == 0 || lf->since_fsync >= lf->fsync_batch) {
        fsync(lf->fd);
        lf->since_fsync = 0;
    }
    pthread_mutex_unlock(&lf->wlock);
    if (out_offset) *out_offset = (uint64_t)off;
    return 0;
}

int log_read(LogFile *lf, uint64_t offset, uint8_t **out, size_t *out_len) {
    uint8_t hdr[LOG_FRAME_HEADER];
    if (read_full(lf->fd, (off_t)offset, hdr, LOG_FRAME_HEADER) != 0) return -1;
    uint32_t crc = get_u32le(hdr);
    uint32_t len = get_u32le(hdr + 4);
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) return -1;
    if (read_full(lf->fd, (off_t)offset + LOG_FRAME_HEADER, buf, len) != 0) {
        free(buf);
        return -1;
    }
    if (crc32_compute(buf, len) != crc) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = len;
    return 0;
}

int log_truncate(LogFile *lf, uint64_t valid_end) {
    pthread_mutex_lock(&lf->wlock);
    int rv = ftruncate(lf->fd, (off_t)valid_end);
    if (rv == 0) lf->size = (off_t)valid_end;
    pthread_mutex_unlock(&lf->wlock);
    return rv;
}

int log_scan(LogFile *lf, log_scan_cb cb, void *ctx, uint64_t *out_valid_end) {
    off_t off = 0;
    off_t end = lf->size;
    while (off + LOG_FRAME_HEADER <= end) {
        uint8_t hdr[LOG_FRAME_HEADER];
        if (read_full(lf->fd, off, hdr, LOG_FRAME_HEADER) != 0) break;
        uint32_t crc = get_u32le(hdr);
        uint32_t len = get_u32le(hdr + 4);
        if (off + LOG_FRAME_HEADER + (off_t)len > end) break; /* torn tail */
        uint8_t *buf = malloc(len ? len : 1);
        if (!buf) return -1;
        if (read_full(lf->fd, off + LOG_FRAME_HEADER, buf, len) != 0) {
            free(buf);
            break;
        }
        if (crc32_compute(buf, len) != crc) {
            free(buf);
            break; /* corrupt frame: treat rest as torn */
        }
        int rv = cb ? cb((uint64_t)off, buf, len, ctx) : 0;
        free(buf);
        off += LOG_FRAME_HEADER + (off_t)len;
        if (rv != 0) break;
    }
    if (out_valid_end) *out_valid_end = (uint64_t)off;
    return 0;
}