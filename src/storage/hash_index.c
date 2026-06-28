/* Open-addressing hash index: id -> log location (T012, snapshot T024). */
#include "aegisdb/hash_index.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/crc32.h"

#define IDX_VERSION 3u
#define IDX_HDR 36   /* "AIDX"(4) ver(4) count(8) covered(8) next_id(8) crc(4) */
#define IDX_ENTRY 22 /* id(8) offset(8) length(4) type(1) deleted(1) */

static int write_all(int fd, const uint8_t *buf, size_t n) {
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

static int read_all(int fd, uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r == 0) return 1; /* short file */
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

#define INITIAL_CAP 1024
#define MAX_LOAD 0.7

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

HashIndex *hash_index_create(void) {
    HashIndex *h = malloc(sizeof(*h));
    if (!h) return NULL;
    h->cap = INITIAL_CAP;
    h->count = 0;
    h->buckets = calloc(h->cap, sizeof(HashEntry));
    if (!h->buckets) {
        free(h);
        return NULL;
    }
    return h;
}

void hash_index_free(HashIndex *h) {
    if (!h) return;
    free(h->buckets);
    free(h);
}

static HashEntry *find_slot(HashEntry *buckets, size_t cap, uint64_t id) {
    size_t mask = cap - 1;
    size_t i = (size_t)mix64(id) & mask;
    for (;;) {
        HashEntry *e = &buckets[i];
        if (!e->used || e->id == id) return e;
        i = (i + 1) & mask;
    }
}

static int rehash(HashIndex *h, size_t newcap) {
    HashEntry *nb = calloc(newcap, sizeof(HashEntry));
    if (!nb) return -1;
    for (size_t i = 0; i < h->cap; i++) {
        if (h->buckets[i].used) {
            HashEntry *d = find_slot(nb, newcap, h->buckets[i].id);
            *d = h->buckets[i];
        }
    }
    free(h->buckets);
    h->buckets = nb;
    h->cap = newcap;
    return 0;
}

int hash_index_put(HashIndex *h, uint64_t id, uint64_t offset, uint32_t length,
                   uint8_t type, uint8_t deleted) {
    if ((double)(h->count + 1) > (double)h->cap * MAX_LOAD) {
        if (rehash(h, h->cap * 2) != 0) return -1;
    }
    HashEntry *e = find_slot(h->buckets, h->cap, id);
    if (!e->used) {
        h->count++;
        e->used = 1;
        e->id = id;
    }
    e->offset = offset;
    e->length = length;
    e->type = type;
    e->deleted = deleted;
    return 0;
}

const HashEntry *hash_index_get(const HashIndex *h, uint64_t id) {
    HashEntry *e = find_slot(h->buckets, h->cap, id);
    if (e->used && !e->deleted) return e;
    return NULL;
}

size_t hash_index_count(const HashIndex *h) { return h->count; }

/* Checkpoint v3: a 36-byte header followed by `count` 22-byte entries.
 * Header (little-endian): "AIDX", u32 version=3, u64 count, u64 covered_log_size,
 * u64 next_id, u32 crc. The CRC covers the header fields [0,32) AND the entries
 * (skipping the CRC field itself), so header corruption — a bad covered offset,
 * next_id, or count — is detected, not just entry corruption. */
uint8_t *hash_index_serialize(const HashIndex *h, uint64_t covered_log_size,
                              uint64_t next_id, size_t *out_len) {
    size_t len = IDX_HDR + h->count * IDX_ENTRY;
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) return NULL;

    uint8_t *p = buf + IDX_HDR;
    uint64_t written = 0;
    for (size_t i = 0; i < h->cap; i++) {
        if (!h->buckets[i].used) continue;
        const HashEntry *e = &h->buckets[i];
        memcpy(p, &e->id, 8);
        memcpy(p + 8, &e->offset, 8);
        memcpy(p + 16, &e->length, 4);
        p[20] = e->type;
        p[21] = e->deleted;
        p += IDX_ENTRY;
        written++;
    }

    uint32_t ver = IDX_VERSION;
    memcpy(buf, "AIDX", 4);
    memcpy(buf + 4, &ver, 4);
    memcpy(buf + 8, &written, 8);
    memcpy(buf + 16, &covered_log_size, 8);
    memcpy(buf + 24, &next_id, 8);
    /* CRC over header fields [0,32) then the entries (chaining is exact for
     * this CRC), leaving the 4-byte CRC field at [32,36) out. */
    uint32_t crc = crc32_compute(buf, 32);
    crc = crc32_update(crc, buf + IDX_HDR, (size_t)written * IDX_ENTRY);
    memcpy(buf + 32, &crc, 4);
    *out_len = IDX_HDR + (size_t)written * IDX_ENTRY;
    return buf;
}

int hash_index_save(const HashIndex *h, const char *path,
                    uint64_t covered_log_size, uint64_t next_id) {
    size_t len = 0;
    uint8_t *buf = hash_index_serialize(h, covered_log_size, next_id, &len);
    if (!buf) return -1;

    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(buf);
        return -1;
    }
    int ok = write_all(fd, buf, len) == 0 && fsync(fd) == 0;
    close(fd);
    free(buf);
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int hash_index_load(HashIndex *h, const char *path,
                    uint64_t *out_covered_log_size, uint64_t *out_next_id) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    uint8_t hdr[IDX_HDR];
    uint32_t ver;
    if (read_all(fd, hdr, IDX_HDR) != 0 || memcmp(hdr, "AIDX", 4) != 0) {
        close(fd);
        return -1;
    }
    memcpy(&ver, hdr + 4, 4);
    if (ver != IDX_VERSION) { /* older/unknown checkpoint: force a full scan */
        close(fd);
        return -1;
    }
    uint64_t cnt, covered, next_id;
    uint32_t want_crc;
    memcpy(&cnt, hdr + 8, 8);
    memcpy(&covered, hdr + 16, 8);
    memcpy(&next_id, hdr + 24, 8);
    memcpy(&want_crc, hdr + 32, 4);
    if (cnt > (uint64_t)((SIZE_MAX - 1) / IDX_ENTRY)) { /* overflow guard */
        close(fd);
        return -1;
    }

    size_t body = (size_t)cnt * IDX_ENTRY;
    uint8_t *buf = malloc(body ? body : 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    if (read_all(fd, buf, body) != 0) {
        free(buf);
        close(fd);
        return -1;
    }
    /* CRC over the header fields [0,32) then the entries — matching serialize. */
    uint32_t crc = crc32_compute(hdr, 32);
    crc = crc32_update(crc, buf, body);
    if (crc != want_crc) {
        free(buf);
        close(fd);
        return -1;
    }
    close(fd);

    for (uint64_t i = 0; i < cnt; i++) {
        const uint8_t *r = buf + i * IDX_ENTRY;
        uint64_t id, offset;
        uint32_t length;
        memcpy(&id, r, 8);
        memcpy(&offset, r + 8, 8);
        memcpy(&length, r + 16, 4);
        hash_index_put(h, id, offset, length, r[20], r[21]);
    }
    free(buf);
    if (out_covered_log_size) *out_covered_log_size = covered;
    if (out_next_id) *out_next_id = next_id;
    return 0;
}