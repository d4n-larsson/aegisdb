/* Open-addressing hash index: id -> log location (T012, snapshot T024). */
#include "aegisdb/hash_index.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* Snapshot format: ["AIDX", u32 version=1, u64 count, then count entries of
 * (u64 id,u64 offset,u32 length,u8 type,u8 deleted)] all little-endian. */
int hash_index_save(const HashIndex *h, const char *path) {
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    uint8_t hdr[16] = {'A', 'I', 'D', 'X'};
    uint32_t ver = 1;
    memcpy(hdr + 4, &ver, 4);
    uint64_t cnt = h->count;
    memcpy(hdr + 8, &cnt, 8);
    if (fwrite(hdr, 1, 16, f) != 16) goto err;
    for (size_t i = 0; i < h->cap; i++) {
        if (!h->buckets[i].used) continue;
        HashEntry *e = &h->buckets[i];
        uint8_t rec[22];
        memcpy(rec, &e->id, 8);
        memcpy(rec + 8, &e->offset, 8);
        memcpy(rec + 16, &e->length, 4);
        rec[20] = e->type;
        rec[21] = e->deleted;
        if (fwrite(rec, 1, 22, f) != 22) goto err;
    }
    if (fflush(f) != 0) goto err;
    fclose(f);
    if (rename(tmp, path) != 0) return -1;
    return 0;
err:
    fclose(f);
    unlink(tmp);
    return -1;
}

int hash_index_load(HashIndex *h, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t hdr[16];
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "AIDX", 4) != 0) {
        fclose(f);
        return -1;
    }
    uint64_t cnt;
    memcpy(&cnt, hdr + 8, 8);
    for (uint64_t i = 0; i < cnt; i++) {
        uint8_t rec[22];
        if (fread(rec, 1, 22, f) != 22) {
            fclose(f);
            return -1;
        }
        uint64_t id, offset;
        uint32_t length;
        memcpy(&id, rec, 8);
        memcpy(&offset, rec + 8, 8);
        memcpy(&length, rec + 16, 4);
        hash_index_put(h, id, offset, length, rec[20], rec[21]);
    }
    fclose(f);
    return 0;
}