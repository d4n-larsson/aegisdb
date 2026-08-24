/* Fact indexes (ROADMAP 5.2): find records by the triple they assert.
 *
 * A record's fact is subject/predicate/object (see record.h). `search`'s
 * `pattern` filter may bind any non-empty subset of those three, so this module
 * keeps three tables rather than one composite key — a single (subject,
 * predicate) map could not answer "everything about subject 42" or "everything
 * asserted with predicate p" without scanning it:
 *
 *   subject   -> [(predicate, record)]   answers {s}   and {s,p}
 *   object    -> [(predicate, record)]   answers {o}   and {p,o}
 *   predicate -> [record]                answers {p}
 *
 * Predicates are interned. They are a controlled vocabulary by construction —
 * the registry bounds them — so an integer key costs almost nothing and keeps
 * the postings fixed-width. Objects are *not* interned: an object literal is
 * arbitrary caller text, so a table of them would grow without bound.
 *
 * In-RAM and derived: nothing is persisted, and recovery rebuilds all three
 * from the live set exactly as it rebuilds tag/lexical/edge. Keyed on ids and
 * interned predicates rather than log offsets, so compaction is a non-event.
 * Not internally synchronized — callers hold db->index_lock.
 */
#ifndef AEGISDB_FACT_INDEX_H
#define AEGISDB_FACT_INDEX_H

#include <stddef.h>
#include <stdint.h>

#include "aegisdb/record.h"

/* Distinct predicates that can be interned. The registry is meant to keep the
 * vocabulary far below this; the cap exists because a server with no registry
 * configured accepts any predicate, and an unbounded table fed by an untrusted
 * field is a memory leak with extra steps. */
#define FACT_MAX_PREDICATES 4096

/* Longest internable predicate. A longer one is refused rather than truncated:
 * truncating would silently merge two distinct predicates, which is worse than
 * declining to index the fact and saying so. */
#define FACT_MAX_PREDICATE_LEN 64

typedef struct FactIndex FactIndex;

FactIndex *fact_index_create(void);
void fact_index_free(FactIndex *f);

/* Index `record_id`'s fact. `okind` must be FACT_OBJ_ID or FACT_OBJ_STRING,
 * with the matching object argument supplied. Re-adding an identical entry is a
 * no-op returning 0.
 *
 * Returns 0 on success (including the no-op), -1 if the fact could not be
 * indexed — a malformed argument, an over-long predicate, the predicate cap
 * reached, or allocation failure. A -1 leaves the index unchanged, and the
 * caller must treat the fact as unindexed rather than assume partial success.
 *
 * Every entry point tolerates a NULL index as "no fact index configured"
 * (--no-fact-index), so write-path call sites stay unguarded. */
int fact_index_add(FactIndex *f, uint64_t record_id, uint64_t subject,
                   const char *predicate, FactKind okind, uint64_t object_id,
                   const char *object_str);

/* Remove the entry `fact_index_add` would have written for these arguments.
 * Absent entries are ignored. */
void fact_index_remove(FactIndex *f, uint64_t record_id, uint64_t subject,
                       const char *predicate, FactKind okind,
                       uint64_t object_id, const char *object_str);

/* ----- lookups -----------------------------------------------------------
 *
 * Each allocates *out (free with free()) and reports *out_n; both are set to
 * NULL/0 when nothing matches, which is not an error. Results are sorted by
 * record id ascending and deduplicated, so paging over them is stable and a
 * record asserting the same thing twice appears once. Return 0/-1.
 */

/* Records whose fact has this subject. `predicate` NULL matches any. */
int fact_index_by_subject(const FactIndex *f, uint64_t subject,
                          const char *predicate, uint64_t **out, size_t *out_n);

/* Records whose fact has this object. `predicate` NULL matches any. */
int fact_index_by_object(const FactIndex *f, FactKind okind, uint64_t object_id,
                         const char *object_str, const char *predicate,
                         uint64_t **out, size_t *out_n);

/* Records whose fact uses this predicate, whatever its subject and object. */
int fact_index_by_predicate(const FactIndex *f, const char *predicate,
                            uint64_t **out, size_t *out_n);

/* How many records use this predicate. The postings length, so this costs a
 * lookup rather than the copy `fact_index_by_predicate` makes — which matters
 * when the caller wants counts for the whole vocabulary and none of the ids. */
size_t fact_index_predicate_facts(const FactIndex *f, const char *predicate);

/* Every record carrying a fact, whatever it says (ROADMAP 5.3). This is what
 * lets the inference job read the fact set without a corpus scan: a record
 * holds exactly one fact, so the per-predicate postings partition the set and
 * their union is every fact-bearing record and nothing else. The job still has
 * to load each record — depth and confidence live there, not in the index —
 * but it loads O(facts) of them rather than O(live records). */
int fact_index_all_records(const FactIndex *f, uint64_t **out, size_t *out_n);

/* Facts currently indexed. */
size_t fact_index_facts(const FactIndex *f);
/* Distinct predicates carrying at least one live fact — not predicates ever
 * seen, so the number survives a restart that replays the same log. (The intern
 * table behind it never shrinks, so FACT_MAX_PREDICATES bounds predicates ever
 * interned rather than the count reported here. Same distinction, and same
 * reason, as edge_index_kinds.) */
size_t fact_index_predicates(const FactIndex *f);
/* Approximate resident bytes across all three tables. Excludes allocator
 * overhead. */
size_t fact_index_bytes(const FactIndex *f);

#endif /* AEGISDB_FACT_INDEX_H */
