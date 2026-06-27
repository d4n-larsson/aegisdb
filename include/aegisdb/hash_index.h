/* Primary ID -> log-location index (T012, snapshot T024).
 *
 * Open-addressing hash table mapping a record id to its latest log frame. */
#ifndef AEGISDB_HASH_INDEX_H
#define AEGISDB_HASH_INDEX_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t id;
    uint64_t offset; /* frame start offset in the log */
    uint32_t length; /* payload length */
    uint8_t type;    /* MemoryType */
    uint8_t deleted; /* tombstone */
    uint8_t used;    /* slot occupied */
} HashEntry;

typedef struct {
    HashEntry *buckets;
    size_t cap;
    size_t count;
} HashIndex;

HashIndex *hash_index_create(void);
void hash_index_free(HashIndex *h);

/* Insert or update the location for `id`. Returns 0/-1. */
int hash_index_put(HashIndex *h, uint64_t id, uint64_t offset, uint32_t length,
                   uint8_t type, uint8_t deleted);

/* Lookup. Returns a pointer to the live entry or NULL if absent. */
const HashEntry *hash_index_get(const HashIndex *h, uint64_t id);

size_t hash_index_count(const HashIndex *h);

/* Persist a snapshot to `path` (T024) and load it back. The log remains the
 * authoritative source; the snapshot is an optimization. Returns 0/-1. */
int hash_index_save(const HashIndex *h, const char *path);
int hash_index_load(HashIndex *h, const char *path);

#endif /* AEGISDB_HASH_INDEX_H */