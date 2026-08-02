/* Per-record usage feedback: how often a memory is actually recalled, and when
 * it last was.
 *
 * `forget` scores retention as importance × recency, where `importance` is a
 * number the writer guessed once and never revisited. The strongest signal
 * available is free and was previously discarded: which memories retrieval
 * actually surfaces. A fact written a year ago and recalled yesterday is live
 * knowledge; one written last week and never retrieved is not.
 *
 * Shape of the thing, and why:
 *
 * - Counters are atomics, so the *read* path can bump them while holding only
 *   db->index_lock for read.
 * - The table's *structure* changes only on the write path (insert/delete/
 *   recovery), under the index write lock. The read path never allocates and
 *   never inserts: an id with no slot is simply not tracked. That is what keeps
 *   recall free of a lock upgrade or a malloc.
 * - It is NOT derivable from the log — unlike every other index here — so it is
 *   checkpointed to its own file. Losing it would silently change what `forget`
 *   deletes after every restart.
 */
#ifndef AEGISDB_USAGE_INDEX_H
#define AEGISDB_USAGE_INDEX_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct UsageIndex UsageIndex;

UsageIndex *usage_index_create(void);
void usage_index_free(UsageIndex *u);

/* Start tracking `id` (write path, index write lock held). Idempotent. 0/-1. */
int usage_index_track(UsageIndex *u, uint64_t id);

/* Stop tracking `id` and drop its counters (write path, write lock held). */
void usage_index_untrack(UsageIndex *u, uint64_t id);

/* Record one recall of `id` at `now_ms` (read path, index read lock held).
 * Takes a const index because it mutates only atomic counters in a slot whose
 * existence is pinned by the read lock. A no-op for an untracked id. */
void usage_index_record(const UsageIndex *u, uint64_t id, uint64_t now_ms);

/* Read `id`'s counters. Returns 0 and fills the outputs (either may be NULL),
 * or -1 when the id is not tracked. */
int usage_index_get(const UsageIndex *u, uint64_t id, uint32_t *out_count,
                    uint64_t *out_last_ms);

/* Tracked records, and approximate resident bytes. */
size_t usage_index_count(const UsageIndex *u);
size_t usage_index_bytes(const UsageIndex *u);

/* Total recalls observed across every tracked record (for `stats`). */
uint64_t usage_index_total_recalls(const UsageIndex *u);

/* Serialize to a freshly malloc'd checkpoint image (caller frees), and load one
 * back. The image carries only ids with a non-zero count — a tracked-but-never-
 * recalled record has nothing worth persisting, and the write path re-adds its
 * slot at recovery. Loading merges into the existing table, so recovery can
 * populate slots from the log first and then restore counters onto them; an id
 * in the image that is no longer live is dropped. Returns NULL / -1 on failure,
 * which callers treat as "no usage history" rather than an error — the counters
 * are a heuristic, never correctness. */
uint8_t *usage_index_serialize(const UsageIndex *u, size_t *out_len);
int usage_index_load_buf(UsageIndex *u, const uint8_t *buf, size_t len);

#endif /* AEGISDB_USAGE_INDEX_H */