/* Volatile working memory: per-session ring buffer with TTL (T041, T042).
 *
 * Working records live only in RAM. Each session is a fixed-capacity ring
 * buffer; adding past capacity evicts the oldest entry. Entries expire after
 * their TTL and are removed lazily on access and by working_store_sweep(). */
#ifndef AEGISDB_WORKING_BUFFER_H
#define AEGISDB_WORKING_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#include "aegisdb/record.h"

typedef struct WorkingStore WorkingStore;

WorkingStore *working_store_create(uint32_t capacity, uint64_t default_ttl_ms);
void working_store_free(WorkingStore *ws);

/* Add `rec` (deep-copied) to the session, assigning a session-local id.
 * ttl_ms == 0 uses the store default. Returns 0/-1; sets *out_id. */
int working_store_add(WorkingStore *ws, const char *session_id,
                      const MemoryRecord *rec, uint64_t now, uint64_t ttl_ms,
                      uint64_t *out_id);

/* Fetch a clone of a live entry (NULL if missing/expired). Caller frees. */
MemoryRecord *working_store_get(WorkingStore *ws, const char *session_id,
                                uint64_t id, uint64_t now);

/* Remove a live entry, moving its contents into *out (caller record_free's).
 * Returns 0 on success, -1 if missing/expired. Used by promote. */
int working_store_take(WorkingStore *ws, const char *session_id, uint64_t id,
                       uint64_t now, MemoryRecord *out);

/* Remove all expired entries across sessions. Returns count removed. */
size_t working_store_sweep(WorkingStore *ws, uint64_t now);

/* Number of occupied slots across all sessions. May include expired entries
 * not yet swept; it is an operational gauge, not an exact live count. */
size_t working_store_count(const WorkingStore *ws);

#endif /* AEGISDB_WORKING_BUFFER_H */