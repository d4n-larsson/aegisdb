/* Temporal index for range queries (T027).
 *
 * Keyed by (created, id) for stable chronological ordering. Implemented as a
 * sorted, dynamically grown array supporting O(log n) range lookup and
 * append-friendly inserts (the common case: monotonically increasing time).
 * This satisfies the temporal range-query contract; a full on-disk B+ tree is
 * a future scaling optimization (see plan.md / research R-006). */
#ifndef AEGISDB_TIME_INDEX_H
#define AEGISDB_TIME_INDEX_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t created;
    uint64_t id;
} TimeEntry;

typedef struct {
    TimeEntry *e;
    size_t n;
    size_t cap;
} TimeIndex;

TimeIndex *time_index_create(void);
void time_index_free(TimeIndex *t);

/* Approximate resident bytes of the index (the sorted entry array). */
size_t time_index_bytes(const TimeIndex *t);

int time_index_add(TimeIndex *t, uint64_t created, uint64_t id);
void time_index_remove(TimeIndex *t, uint64_t created, uint64_t id);

/* Return up to `max` ids whose created time is in [start,end], in chronological
 * order. Allocates *out_ids (free with free()). Returns 0/-1. */
int time_index_range(const TimeIndex *t, uint64_t start, uint64_t end,
                     size_t max, uint64_t **out_ids, size_t *out_n);

/* Like time_index_range but, when the range holds more than `max` entries,
 * keeps the most-recent `max` (the tail) rather than the oldest — so a bounded
 * broad scan never hides newly written records. `max` == 0 means unlimited.
 * *truncated (may be NULL) is set to 1 iff entries were dropped. Result is still
 * in chronological (ascending) order. Allocates *out_ids. Returns 0/-1. */
int time_index_range_recent(const TimeIndex *t, uint64_t start, uint64_t end,
                            size_t max, uint64_t **out_ids, size_t *out_n,
                            int *truncated);

#endif /* AEGISDB_TIME_INDEX_H */