/* Append-only log: magic + CRC framed records, with resync-on-corruption
 * recovery and in-place migration of the legacy v1 format. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aegisdb/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/crc32.h"
#include "aegisdb/logging.h"

#define LOG_FRAME_MAGIC 0xA3D1B70Fu /* sync marker at the start of every frame */
#define V1_FRAME_HEADER 8           /* legacy: crc(4) + len(4), crc over payload */

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

/* Fill a 16-byte v2 frame header for `payload`/`len` into `hdr`. */
static void build_header(uint8_t *hdr, size_t len, uint32_t payload_crc) {
    put_u32le(hdr, LOG_FRAME_MAGIC);
    put_u32le(hdr + 4, (uint32_t)len);
    put_u32le(hdr + 8, payload_crc);
    put_u32le(hdr + 12, crc32_compute(hdr, 12));
}

/* Validate a 16-byte header in `hdr`. Returns 0 and writes the length and
 * payload CRC out when the magic and header CRC are intact; -1 otherwise. */
static int parse_header(const uint8_t *hdr, uint32_t *len, uint32_t *pcrc) {
    if (get_u32le(hdr) != LOG_FRAME_MAGIC) return -1;
    if (get_u32le(hdr + 12) != crc32_compute(hdr, 12)) return -1;
    *len = get_u32le(hdr + 4);
    *pcrc = get_u32le(hdr + 8);
    return 0;
}

/* Rewrite a legacy v1 log (8-byte [crc][len] frames, no magic) at `path` into
 * the v2 frame format, stopping at the first torn/corrupt v1 frame. */
static int migrate_legacy_log(const char *path) {
    int oldfd = open(path, O_RDONLY);
    if (oldfd < 0) return -1;
    off_t size = lseek(oldfd, 0, SEEK_END);
    if (size < 0) {
        close(oldfd);
        return -1;
    }

    char newpath[1200];
    snprintf(newpath, sizeof(newpath), "%s.migrate", path);
    int nfd = open(newpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (nfd < 0) {
        close(oldfd);
        return -1;
    }

    off_t in = 0, out = 0;
    long migrated = 0;
    int failed = 0;
    while (in + V1_FRAME_HEADER <= size) {
        uint8_t h[V1_FRAME_HEADER];
        if (read_full(oldfd, in, h, V1_FRAME_HEADER) != 0) break;
        uint32_t crc = get_u32le(h);
        uint32_t len = get_u32le(h + 4);
        if (in + V1_FRAME_HEADER + (off_t)len > size) break; /* torn tail */
        uint8_t *buf = malloc(len ? len : 1);
        if (!buf) {
            failed = 1;
            break;
        }
        if (read_full(oldfd, in + V1_FRAME_HEADER, buf, len) != 0 ||
            crc32_compute(buf, len) != crc) {
            free(buf);
            break; /* stop migrating at the first damaged v1 frame */
        }
        uint8_t nh[LOG_FRAME_HEADER];
        build_header(nh, len, crc);
        if (write_full(nfd, out, nh, LOG_FRAME_HEADER) != 0 ||
            write_full(nfd, out + LOG_FRAME_HEADER, buf, len) != 0) {
            free(buf);
            failed = 1;
            break;
        }
        free(buf);
        out += LOG_FRAME_HEADER + (off_t)len;
        in += V1_FRAME_HEADER + (off_t)len;
        migrated++;
    }

    /* A non-empty legacy log that yielded no frames is corrupt at the head;
     * refuse rather than rename an empty file over it and destroy the data. */
    if (migrated == 0 && size > 0) {
        LOG_ERROR("log: legacy log %s has no recoverable v1 frames; not "
                  "migrating (original preserved for inspection)", path);
        failed = 1;
    }

    if (failed || fsync(nfd) != 0) {
        close(nfd);
        close(oldfd);
        unlink(newpath);
        return -1;
    }
    close(nfd);
    close(oldfd);
    if (rename(newpath, path) != 0) {
        unlink(newpath);
        return -1;
    }
    LOG_INFO("log: migrated %ld legacy frame(s) to v2 format", migrated);
    return 0;
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
    if (end < 0) goto fail;

    /* A non-empty log whose first bytes are not the v2 magic is a legacy v1
     * log; migrate it in place so existing data survives the upgrade. */
    if (end >= 4) {
        uint8_t m[4];
        if (read_full(lf->fd, 0, m, 4) == 0 &&
            get_u32le(m) != LOG_FRAME_MAGIC) {
            close(lf->fd);
            if (migrate_legacy_log(path) != 0) {
                LOG_ERROR("log: failed to migrate legacy log %s", path);
                pthread_mutex_destroy(&lf->wlock);
                return -1;
            }
            lf->fd = open(path, O_RDWR, 0644);
            if (lf->fd < 0) {
                pthread_mutex_destroy(&lf->wlock);
                return -1;
            }
            end = lseek(lf->fd, 0, SEEK_END);
            if (end < 0) goto fail;
        }
    }
    lf->size = end;
    return 0;

fail:
    close(lf->fd);
    lf->fd = -1;
    pthread_mutex_destroy(&lf->wlock);
    return -1;
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
    if (lf->fd < 0) return;
    pthread_mutex_lock(&lf->wlock);
    fsync(lf->fd);
    lf->since_fsync = 0;
    pthread_mutex_unlock(&lf->wlock);
}

int log_flush_pending(const LogFile *lf) {
    return lf->fd >= 0 && lf->since_fsync > 0;
}

int log_append(LogFile *lf, const uint8_t *payload, size_t len,
               uint64_t *out_offset) {
    if (len > 0xFFFFFFFFu) return -1;
    uint8_t hdr[LOG_FRAME_HEADER];
    build_header(hdr, len, crc32_compute(payload, len));

    pthread_mutex_lock(&lf->wlock);
    off_t off = lf->size;
    if (write_full(lf->fd, off, hdr, LOG_FRAME_HEADER) != 0 ||
        write_full(lf->fd, off + LOG_FRAME_HEADER, payload, len) != 0) {
        pthread_mutex_unlock(&lf->wlock);
        return -1;
    }
    lf->size = off + LOG_FRAME_HEADER + (off_t)len;
    lf->since_fsync++;
    /* The fsync is deferred to log_fsync_if_batched(), which the caller invokes
     * after releasing the index lock, so a durable write does not hold that lock
     * across the fsync. */
    pthread_mutex_unlock(&lf->wlock);
    if (out_offset) *out_offset = (uint64_t)off;
    return 0;
}

/* fsync when the configured batch threshold is reached (sync = every write,
 * batch = every fsync_batch writes; interval never triggers here — the
 * maintenance thread flushes on a timer). Group-commit safe: one fsync covers
 * every frame appended since the last flush, and `since_fsync` is shared under
 * the log mutex, so a writer that finds the counter already reset was made
 * durable by a concurrent writer's fsync. */
void log_fsync_if_batched(LogFile *lf) {
    if (lf->fd < 0) return;
    pthread_mutex_lock(&lf->wlock);
    if (lf->fsync_batch == 0 || lf->since_fsync >= lf->fsync_batch) {
        fsync(lf->fd);
        lf->since_fsync = 0;
    }
    pthread_mutex_unlock(&lf->wlock);
}

int log_read(LogFile *lf, uint64_t offset, uint8_t **out, size_t *out_len) {
    uint8_t hdr[LOG_FRAME_HEADER];
    if (read_full(lf->fd, (off_t)offset, hdr, LOG_FRAME_HEADER) != 0) return -1;
    uint32_t len, pcrc;
    if (parse_header(hdr, &len, &pcrc) != 0) return -1;
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) return -1;
    if (read_full(lf->fd, (off_t)offset + LOG_FRAME_HEADER, buf, len) != 0) {
        free(buf);
        return -1;
    }
    if (crc32_compute(buf, len) != pcrc) {
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

/* Find the next byte offset >= `from` that begins a structurally valid frame
 * (magic + header CRC + a payload that fits before `end`). Returns `end` if no
 * such frame exists. Scans in windows to avoid a syscall per byte. */
static off_t find_next_frame(int fd, off_t from, off_t end) {
    enum { WIN = 65536 };
    uint8_t win[WIN];
    for (off_t base = from; base + LOG_FRAME_HEADER <= end;) {
        size_t want = (size_t)(end - base);
        if (want > WIN) want = WIN;
        if (read_full(fd, base, win, want) != 0) break;
        /* Candidate positions where a full header could still fit in `end`. */
        size_t limit = want >= LOG_FRAME_HEADER ? want - LOG_FRAME_HEADER : 0;
        for (size_t i = 0; i <= limit; i++) {
            if (get_u32le(win + i) != LOG_FRAME_MAGIC) continue;
            off_t off = base + (off_t)i;
            uint8_t hdr[LOG_FRAME_HEADER];
            uint32_t len, pcrc;
            if (read_full(fd, off, hdr, LOG_FRAME_HEADER) != 0) continue;
            if (parse_header(hdr, &len, &pcrc) != 0) continue;
            if (off + LOG_FRAME_HEADER + (off_t)len > end) continue;
            return off;
        }
        /* Advance, overlapping by the header size so a magic straddling the
         * window boundary is not missed. */
        base += (off_t)(want - (LOG_FRAME_HEADER - 1));
    }
    return end;
}

int log_scan(LogFile *lf, uint64_t start, log_scan_cb cb, void *ctx,
             LogScanResult *out) {
    off_t off = (off_t)start;
    off_t end = lf->size;
    uint64_t clean_end = start; /* [0, start) is assumed clean (e.g. checkpoint) */
    size_t good = 0, corrupt = 0;
    int hole = 0, recovered_after_hole = 0;

    while (off < end) {
        uint8_t hdr[LOG_FRAME_HEADER];
        if (off + LOG_FRAME_HEADER > end ||
            read_full(lf->fd, off, hdr, LOG_FRAME_HEADER) != 0)
            break; /* trailing partial header: torn tail */

        uint32_t len, pcrc;
        if (parse_header(hdr, &len, &pcrc) != 0) {
            /* Damaged or misaligned header: resync to the next valid frame. */
            off_t nxt = find_next_frame(lf->fd, off + 1, end);
            if (nxt >= end) break; /* nothing recoverable ahead: torn tail */
            hole = 1;
            corrupt++;
            off = nxt;
            continue;
        }
        if (off + LOG_FRAME_HEADER + (off_t)len > end)
            break; /* header valid but payload incomplete: torn tail */

        uint8_t *buf = malloc(len ? len : 1);
        if (!buf) return -1;
        if (read_full(lf->fd, off + LOG_FRAME_HEADER, buf, len) != 0) {
            free(buf);
            break;
        }
        if (crc32_compute(buf, len) != pcrc) {
            /* Header trusted (its CRC matched), so the length is reliable: skip
             * exactly this frame and continue. */
            free(buf);
            hole = 1;
            corrupt++;
            off += LOG_FRAME_HEADER + (off_t)len;
            continue;
        }

        int rv = cb ? cb((uint64_t)off, buf, len, ctx) : 0;
        free(buf);
        good++;
        off += LOG_FRAME_HEADER + (off_t)len;
        if (hole)
            recovered_after_hole = 1;
        else
            clean_end = (uint64_t)off;
        if (rv != 0) break;
    }

    if (out) {
        /* Preserve recovered frames that follow a mid-stream hole; otherwise the
         * tail is torn and is trimmed back to the last clean frame. */
        out->truncate_to = recovered_after_hole ? (uint64_t)end : clean_end;
        out->good_frames = good;
        out->corrupt_frames = corrupt;
        out->recovered_after_hole = recovered_after_hole;
    }
    return 0;
}