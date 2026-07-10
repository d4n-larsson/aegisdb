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
#include <sys/random.h>
#include <unistd.h>

#include "aegisdb/aead.h"
#include "aegisdb/crc32.h"
#include "aegisdb/logging.h"

#define LOG_FRAME_MAGIC 0xA3D1B70Fu /* v2 plaintext frame sync marker */
#define LOG_FRAME_MAGIC_ENC 0xA3D1B71Eu /* v3 encrypted (AEAD) frame sync marker */
#define V1_FRAME_HEADER 8           /* legacy: crc(4) + len(4), crc over payload */
/* v3: magic(4) + len(4) + nonce(24) + hdr_crc(4); then ciphertext(len) + tag. */
#define V3_NONCE_OFF 8
#define V3_FRAME_HEADER 36
#define V3_AAD_LEN 32 /* magic+len+nonce: authenticated as AEAD associated data */
#define V3_FRAME_OVERHEAD (V3_FRAME_HEADER + AEAD_TAG_LEN)

/* Fill `n` bytes with cryptographic randomness (frame nonces). */
static int fill_random(uint8_t *p, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(p + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

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

/* Build a 36-byte v3 encrypted-frame header for `len`/`nonce` into `hdr`. The
 * first 32 bytes (magic+len+nonce) are both HEADER_CRC-covered and used as the
 * AEAD associated data. */
static void build_header_v3(uint8_t *hdr, size_t len, const uint8_t nonce[24]) {
    put_u32le(hdr, LOG_FRAME_MAGIC_ENC);
    put_u32le(hdr + 4, (uint32_t)len);
    memcpy(hdr + V3_NONCE_OFF, nonce, 24);
    put_u32le(hdr + 32, crc32_compute(hdr, 32));
}

/* Validate a 36-byte v3 header (magic + HEADER_CRC); write the payload length.
 * The nonce is at hdr+V3_NONCE_OFF. Returns 0/-1. */
static int parse_header_v3(const uint8_t *hdr, uint32_t *len) {
    if (get_u32le(hdr) != LOG_FRAME_MAGIC_ENC) return -1;
    if (get_u32le(hdr + 32) != crc32_compute(hdr, 32)) return -1;
    *len = get_u32le(hdr + 4);
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

/* Try to AEAD-open the first frame of an encrypted log to confirm the key is the
 * one it was sealed with. Returns 1 (verified), 0 (wrong key / unreadable). An
 * empty log trivially verifies. */
static int verify_first_frame(LogFile *lf, off_t end) {
    if (end < V3_FRAME_HEADER) return 1; /* nothing sealed yet */
    uint8_t hdr[V3_FRAME_HEADER];
    if (read_full(lf->fd, 0, hdr, V3_FRAME_HEADER) != 0) return 0;
    uint32_t len;
    if (parse_header_v3(hdr, &len) != 0) return 0;
    if ((off_t)V3_FRAME_HEADER + (off_t)len + AEAD_TAG_LEN > end) return 0;
    uint8_t *ct = malloc(len ? len : 1);
    uint8_t *pt = malloc(len ? len : 1);
    uint8_t tag[AEAD_TAG_LEN];
    int ok = 0;
    if (ct && pt &&
        read_full(lf->fd, V3_FRAME_HEADER, ct, len) == 0 &&
        read_full(lf->fd, (off_t)V3_FRAME_HEADER + len, tag, AEAD_TAG_LEN) == 0)
        ok = aead_open(lf->key, hdr + V3_NONCE_OFF, hdr, V3_AAD_LEN, ct, len, pt,
                       tag) == 0;
    free(ct);
    free(pt);
    return ok;
}

int log_open(LogFile *lf, const char *path, size_t fsync_batch,
             const uint8_t *key, LogOpenStatus *status) {
    if (status) *status = LOG_OPEN_ERR_IO;
    memset(lf, 0, sizeof(*lf));
    strncpy(lf->path, path, sizeof(lf->path) - 1);
    lf->fsync_batch = fsync_batch;
    if (key) {
        lf->encrypted = 1;
        memcpy(lf->key, key, AEAD_KEY_LEN);
    }
    if (pthread_mutex_init(&lf->wlock, NULL) != 0) return -1;
    lf->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (lf->fd < 0) {
        pthread_mutex_destroy(&lf->wlock);
        return -1;
    }
    off_t end = lseek(lf->fd, 0, SEEK_END);
    if (end < 0) goto fail;

    /* Reconcile the on-disk mode with the key argument (fail-closed). An empty
     * log adopts the requested mode; a non-empty one must match. */
    if (end >= 4) {
        uint8_t m[4];
        if (read_full(lf->fd, 0, m, 4) != 0) goto fail;
        uint32_t magic = get_u32le(m);
        int on_disk_encrypted = (magic == LOG_FRAME_MAGIC_ENC);
        int on_disk_plain_v2 = (magic == LOG_FRAME_MAGIC);

        if (on_disk_encrypted && !key) {
            LOG_ERROR("log: %s is encrypted but no --encryption-key-file was "
                      "given", path);
            if (status) *status = LOG_OPEN_ERR_PLAIN_ON_ENC;
            goto fail_quiet;
        }
        if (on_disk_plain_v2 && key) {
            LOG_ERROR("log: %s is plaintext but a key was given; run "
                      "--encrypt-migrate to convert it", path);
            if (status) *status = LOG_OPEN_ERR_KEY_ON_PLAIN;
            goto fail_quiet;
        }
        if (!on_disk_encrypted && !on_disk_plain_v2) {
            /* Legacy v1 (no magic). Migration produces a plaintext v2 log, so it
             * is incompatible with a key; refuse rather than silently downgrade. */
            if (key) {
                LOG_ERROR("log: %s is a legacy plaintext log; migrate it to v2 "
                          "unencrypted first, then --encrypt-migrate", path);
                if (status) *status = LOG_OPEN_ERR_KEY_ON_PLAIN;
                goto fail_quiet;
            }
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
        } else if (on_disk_encrypted && !verify_first_frame(lf, end)) {
            LOG_ERROR("log: the given key does not decrypt %s (wrong key)", path);
            if (status) *status = LOG_OPEN_ERR_WRONG_KEY;
            goto fail_quiet;
        }
    }
    lf->size = end;
    if (status) *status = LOG_OPEN_OK;
    return 0;

fail:
    if (status) *status = LOG_OPEN_ERR_IO;
fail_quiet:
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

/* Append an encrypted v3 frame: header || AEAD(ciphertext) || tag, with a fresh
 * random nonce and the header prefix as associated data. */
static int log_append_encrypted(LogFile *lf, const uint8_t *payload, size_t len,
                                uint64_t *out_offset) {
    uint8_t hdr[V3_FRAME_HEADER];
    uint8_t nonce[AEAD_NONCE_LEN];
    if (fill_random(nonce, sizeof nonce) != 0) return -1;
    build_header_v3(hdr, len, nonce);
    uint8_t *ct = malloc(len ? len : 1);
    if (!ct) return -1;
    uint8_t tag[AEAD_TAG_LEN];
    aead_seal(lf->key, nonce, hdr, V3_AAD_LEN, payload, len, ct, tag);

    pthread_mutex_lock(&lf->wlock);
    off_t off = lf->size;
    if (write_full(lf->fd, off, hdr, V3_FRAME_HEADER) != 0 ||
        write_full(lf->fd, off + V3_FRAME_HEADER, ct, len) != 0 ||
        write_full(lf->fd, off + V3_FRAME_HEADER + (off_t)len, tag,
                   AEAD_TAG_LEN) != 0) {
        pthread_mutex_unlock(&lf->wlock);
        free(ct);
        return -1;
    }
    lf->size = off + (off_t)V3_FRAME_OVERHEAD + (off_t)len;
    lf->since_fsync++;
    pthread_mutex_unlock(&lf->wlock);
    free(ct);
    if (out_offset) *out_offset = (uint64_t)off;
    return 0;
}

int log_append(LogFile *lf, const uint8_t *payload, size_t len,
               uint64_t *out_offset) {
    if (len > 0xFFFFFFFFu) return -1;
    if (lf->encrypted) return log_append_encrypted(lf, payload, len, out_offset);
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

/* Read + decrypt a v3 frame at `offset`. Returns 0/-1 (auth failure -> -1). */
static int log_read_encrypted(LogFile *lf, uint64_t offset, uint8_t **out,
                              size_t *out_len) {
    uint8_t hdr[V3_FRAME_HEADER];
    if (read_full(lf->fd, (off_t)offset, hdr, V3_FRAME_HEADER) != 0) return -1;
    uint32_t len;
    if (parse_header_v3(hdr, &len) != 0) return -1;
    uint8_t *buf = malloc(len ? len : 1);
    uint8_t tag[AEAD_TAG_LEN];
    if (!buf) return -1;
    if (read_full(lf->fd, (off_t)offset + V3_FRAME_HEADER, buf, len) != 0 ||
        read_full(lf->fd, (off_t)offset + V3_FRAME_HEADER + len, tag,
                  AEAD_TAG_LEN) != 0) {
        free(buf);
        return -1;
    }
    /* Decrypt in place; AAD is the header prefix (authenticates len + nonce). */
    if (aead_open(lf->key, hdr + V3_NONCE_OFF, hdr, V3_AAD_LEN, buf, len, buf,
                  tag) != 0) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = len;
    return 0;
}

int log_read(LogFile *lf, uint64_t offset, uint8_t **out, size_t *out_len) {
    if (lf->encrypted) return log_read_encrypted(lf, offset, out, out_len);
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

size_t log_frame_overhead(const LogFile *lf) {
    return lf->encrypted ? V3_FRAME_OVERHEAD : LOG_FRAME_HEADER;
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
 * such frame exists. Scans in windows to avoid a syscall per byte. Mode-aware:
 * an encrypted log resyncs on the v3 magic/header, a plaintext one on v2. Resync
 * is structural only — no decryption — so it needs no key. */
static off_t find_next_frame(const LogFile *lf, off_t from, off_t end) {
    const int enc = lf->encrypted;
    const uint32_t want_magic = enc ? LOG_FRAME_MAGIC_ENC : LOG_FRAME_MAGIC;
    const off_t hdr_len = enc ? V3_FRAME_HEADER : LOG_FRAME_HEADER;
    const off_t overhead = enc ? V3_FRAME_OVERHEAD : LOG_FRAME_HEADER;
    enum { WIN = 65536 };
    uint8_t win[WIN];
    for (off_t base = from; base + hdr_len <= end;) {
        size_t want = (size_t)(end - base);
        if (want > WIN) want = WIN;
        if (read_full(lf->fd, base, win, want) != 0) break;
        size_t limit = want >= (size_t)hdr_len ? want - (size_t)hdr_len : 0;
        for (size_t i = 0; i <= limit; i++) {
            if (get_u32le(win + i) != want_magic) continue;
            off_t off = base + (off_t)i;
            uint8_t hdr[V3_FRAME_HEADER];
            uint32_t len, pcrc;
            if (read_full(lf->fd, off, hdr, hdr_len) != 0) continue;
            if (enc ? (parse_header_v3(hdr, &len) != 0)
                    : (parse_header(hdr, &len, &pcrc) != 0))
                continue;
            if (off + overhead + (off_t)len > end) continue;
            return off;
        }
        /* Advance, overlapping by the header size so a magic straddling the
         * window boundary is not missed. */
        base += (off_t)(want - (size_t)(hdr_len - 1));
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

    const int enc = lf->encrypted;
    const off_t hdr_len = enc ? V3_FRAME_HEADER : LOG_FRAME_HEADER;
    const off_t overhead = enc ? V3_FRAME_OVERHEAD : LOG_FRAME_HEADER;

    while (off < end) {
        uint8_t hdr[V3_FRAME_HEADER];
        if (off + hdr_len > end ||
            read_full(lf->fd, off, hdr, hdr_len) != 0)
            break; /* trailing partial header: torn tail */

        uint32_t len, pcrc;
        int hdr_ok = enc ? (parse_header_v3(hdr, &len) == 0)
                         : (parse_header(hdr, &len, &pcrc) == 0);
        if (!hdr_ok) {
            /* Damaged or misaligned header: resync to the next valid frame. */
            off_t nxt = find_next_frame(lf, off + 1, end);
            if (nxt >= end) break; /* nothing recoverable ahead: torn tail */
            hole = 1;
            corrupt++;
            off = nxt;
            continue;
        }
        if (off + overhead + (off_t)len > end)
            break; /* header valid but payload/tag incomplete: torn tail */

        uint8_t *buf = malloc(len ? len : 1);
        if (!buf) return -1;
        int bad = 0;
        if (enc) {
            uint8_t tag[AEAD_TAG_LEN];
            if (read_full(lf->fd, off + hdr_len, buf, len) != 0 ||
                read_full(lf->fd, off + hdr_len + (off_t)len, tag,
                          AEAD_TAG_LEN) != 0) {
                free(buf);
                break;
            }
            /* AEAD tag is the integrity check (len+nonce are header-trusted, so
             * the frame boundary is reliable even when authentication fails). */
            bad = aead_open(lf->key, hdr + V3_NONCE_OFF, hdr, V3_AAD_LEN, buf,
                            len, buf, tag) != 0;
        } else {
            if (read_full(lf->fd, off + hdr_len, buf, len) != 0) {
                free(buf);
                break;
            }
            bad = crc32_compute(buf, len) != pcrc;
        }
        if (bad) {
            /* Header trusted, so the length is reliable: skip exactly this frame. */
            free(buf);
            hole = 1;
            corrupt++;
            off += overhead + (off_t)len;
            continue;
        }

        int rv = cb ? cb((uint64_t)off, buf, len, ctx) : 0;
        free(buf);
        good++;
        off += overhead + (off_t)len;
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