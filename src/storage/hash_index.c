/* Open-addressing hash index: id -> log location (T012, snapshot T024). */
#include "aegisdb/hash_index.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/ckpt_crypt.h"
#include "aegisdb/crc32.h"
#include "aegisdb/hash_mix.h"

#define IDX_VERSION 4u
#define IDX_HDR 36   /* "AIDX"(4) ver(4) count(8) covered(8) next_id(8) crc(4) */
#define IDX_ENTRY 30 /* id(8) offset(8) length(4) type(1) deleted(1) expires_at(8) */

#define INITIAL_CAP 1024
#define MAX_LOAD 0.7

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
                   uint8_t type, uint8_t deleted, uint64_t expires_at) {
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
    e->expires_at = expires_at;
    return 0;
}

const HashEntry *hash_index_get(const HashIndex *h, uint64_t id) {
    HashEntry *e = find_slot(h->buckets, h->cap, id);
    if (e->used && !e->deleted) return e;
    return NULL;
}

size_t hash_index_bytes(const HashIndex *h) {
    return h ? sizeof(*h) + h->cap * sizeof(HashEntry) : 0;
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
        memcpy(p + 22, &e->expires_at, 8);
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
                    uint64_t covered_log_size, uint64_t next_id,
                    const uint8_t *key) {
    size_t len = 0;
    uint8_t *buf = hash_index_serialize(h, covered_log_size, next_id, &len);
    if (!buf) return -1;
    int rv = ckpt_write(path, key, buf, len);
    free(buf);
    return rv;
}

int hash_index_load(HashIndex *h, const char *path,
                    uint64_t *out_covered_log_size, uint64_t *out_next_id,
                    const uint8_t *key) {
    /* ckpt_read yields the plaintext checkpoint (decrypting first when keyed);
     * a wrong key / tamper / missing file all return -1, so recovery falls back
     * to a full log scan. */
    uint8_t *file = NULL;
    size_t flen = 0;
    if (ckpt_read(path, key, &file, &flen) != 0) return -1;
    if (flen < IDX_HDR || memcmp(file, "AIDX", 4) != 0) {
        free(file);
        return -1;
    }
    uint32_t ver;
    memcpy(&ver, file + 4, 4);
    if (ver != IDX_VERSION) { /* older/unknown checkpoint: force a full scan */
        free(file);
        return -1;
    }
    uint64_t cnt, covered, next_id;
    uint32_t want_crc;
    memcpy(&cnt, file + 8, 8);
    memcpy(&covered, file + 16, 8);
    memcpy(&next_id, file + 24, 8);
    memcpy(&want_crc, file + 32, 4);
    if (cnt > (uint64_t)((SIZE_MAX - 1) / IDX_ENTRY)) { /* overflow guard */
        free(file);
        return -1;
    }
    size_t body = (size_t)cnt * IDX_ENTRY;
    if (flen != IDX_HDR + body) { /* declared count must match the file size */
        free(file);
        return -1;
    }
    const uint8_t *entries = file + IDX_HDR;
    /* CRC over the header fields [0,32) then the entries — matching serialize. */
    uint32_t crc = crc32_compute(file, 32);
    crc = crc32_update(crc, entries, body);
    if (crc != want_crc) {
        free(file);
        return -1;
    }

    for (uint64_t i = 0; i < cnt; i++) {
        const uint8_t *r = entries + i * IDX_ENTRY;
        uint64_t id, offset, expires_at;
        uint32_t length;
        memcpy(&id, r, 8);
        memcpy(&offset, r + 8, 8);
        memcpy(&length, r + 16, 4);
        memcpy(&expires_at, r + 22, 8);
        hash_index_put(h, id, offset, length, r[20], r[21], expires_at);
    }
    free(file);
    if (out_covered_log_size) *out_covered_log_size = covered;
    if (out_next_id) *out_next_id = next_id;
    return 0;
}