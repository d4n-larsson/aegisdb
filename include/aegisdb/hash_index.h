/* Primary ID -> log-location index (T012, snapshot T024).
 *
 * Open-addressing hash table mapping a record id to its latest log frame. */
#ifndef AEGISDB_HASH_INDEX_H
#define AEGISDB_HASH_INDEX_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t id;
    uint64_t offset;      /* frame start offset in the log */
    uint64_t expires_at;  /* epoch ms TTL horizon; 0 = never (kept here so the
                           * expiry sweep is an in-memory scan, no record reads) */
    uint32_t length;      /* payload length */
    uint8_t type;         /* MemoryType */
    uint8_t deleted;      /* tombstone */
    uint8_t used;         /* slot occupied */
} HashEntry;

typedef struct {
    HashEntry *buckets;
    size_t cap;
    size_t count;
} HashIndex;

HashIndex *hash_index_create(void);
void hash_index_free(HashIndex *h);

/* Insert or update the location for `id`. `expires_at` is the TTL horizon in
 * epoch ms (0 = never). Returns 0/-1. */
int hash_index_put(HashIndex *h, uint64_t id, uint64_t offset, uint32_t length,
                   uint8_t type, uint8_t deleted, uint64_t expires_at);

/* Lookup. Returns a pointer to the live entry or NULL if absent. */
const HashEntry *hash_index_get(const HashIndex *h, uint64_t id);

size_t hash_index_count(const HashIndex *h);

/* Approximate resident bytes of the table (the bucket array). */
size_t hash_index_bytes(const HashIndex *h);

/* Persist a checkpoint to `path` and load it back. The checkpoint records the
 * log size it reflects (`covered_log_size`) and the id allocator (`next_id`) so
 * recovery can trust [0, covered) and replay only the log tail. The log remains
 * the authoritative source; a missing/corrupt checkpoint just forces a full
 * scan. `key` (AEAD_KEY_LEN bytes, or NULL for plaintext) encrypts the
 * checkpoint at rest; loading with the wrong key returns -1. Returns 0/-1. */
int hash_index_save(const HashIndex *h, const char *path,
                    uint64_t covered_log_size, uint64_t next_id,
                    const uint8_t *key);
int hash_index_load(HashIndex *h, const char *path,
                    uint64_t *out_covered_log_size, uint64_t *out_next_id,
                    const uint8_t *key);

/* Serialize the index into a freshly malloc'd checkpoint image (caller frees).
 * Lets a caller build the image under a lock and write it without one. Returns
 * NULL on allocation failure; sets *out_len on success. */
uint8_t *hash_index_serialize(const HashIndex *h, uint64_t covered_log_size,
                              uint64_t next_id, size_t *out_len);

#endif /* AEGISDB_HASH_INDEX_H */