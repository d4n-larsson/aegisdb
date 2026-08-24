/* The contradictions the last inference pass found (ROADMAP 5.4 §6).
 *
 * `stats.metrics.conflicts` answers one question — does the corpus hold a
 * contradiction right now? — and adjudication needs a different one: *which
 * pair*. There was no way to reach it. A `conflicts_with` edge is only walkable
 * from an id you already hold, and the edge index is keyed target -> sources
 * with no enumeration by kind, so finding the flagged pairs meant scanning
 * every live record.
 *
 * The pass already computes exactly this set on every tick. This keeps it
 * rather than throwing it away, which is the whole design: no scan of its own,
 * no second traversal of the fact set, and no way to disagree with the gauge it
 * sits beside — both are filled by the same loop.
 *
 * Derived and in-RAM like every index here, never persisted, and replaced
 * *whole* each tick rather than accumulated. A conflict that has been resolved
 * since the last pass must stop being reported, and an accumulating list would
 * hand an adjudicator pairs whose records are already tombstoned.
 *
 * Not internally synchronized: the pass builds a private set and the caller
 * swaps it in under db->conflicts_lock, so a reader never sees a half-filled
 * one.
 */
#ifndef AEGISDB_CONFLICT_SET_H
#define AEGISDB_CONFLICT_SET_H

#include <stddef.h>
#include <stdint.h>

/* A predicate is capped at 64 bytes by `insert` (the fact indexes intern it),
 * and a namespace at MAX_AGENT_ID. Inlined rather than pointed at because the
 * set outlives the records the pass loaded: it is read on a later request,
 * against a corpus that may have changed. */
#define CONFLICT_PREDICATE_MAX 64
#define CONFLICT_NS_MAX 128
#define CONFLICT_REASON_MAX 15

/* Pairs retained per tick, server-wide. A bound rather than a growing list,
 * because this is a *report*: an operator or an adjudicator works through
 * contradictions a handful at a time, and a corpus with more than this many is
 * telling you something the list itself cannot fix. Past the cap the set says
 * it was truncated — the gauge stays exact regardless, since it is counted and
 * not stored. */
#define CONFLICT_SET_MAX 1024

typedef struct {
    uint64_t a; /* the two records that contradict each other */
    uint64_t b;
    char ns[CONFLICT_NS_MAX + 1];
    char predicate_a[CONFLICT_PREDICATE_MAX + 1];
    char predicate_b[CONFLICT_PREDICATE_MAX + 1];
    /* "cardinality" (two live values for a single-valued predicate) or
     * "mutex_with" (two predicates the registry says cannot both hold). */
    char reason[CONFLICT_REASON_MAX + 1];
} ConflictPair;

typedef struct ConflictSet ConflictSet;

ConflictSet *conflict_set_create(void);
void conflict_set_free(ConflictSet *cs);

/* Empty it for a fresh pass. Tolerates NULL. */
void conflict_set_clear(ConflictSet *cs);

/* Record one contradiction. Returns 0 when stored, -1 when the set is full or
 * the arguments are unusable; a full set is marked truncated. Tolerates a NULL
 * set so the pass can call it unconditionally. */
int conflict_set_add(ConflictSet *cs, uint64_t a, uint64_t b, const char *ns,
                     const char *predicate_a, const char *predicate_b,
                     const char *reason);

size_t conflict_set_count(const ConflictSet *cs);
int conflict_set_truncated(const ConflictSet *cs);

/* Copy out at most `max` pairs, restricted to namespace `ns` when it is
 * non-NULL and non-empty. `*total` receives how many matched before `max` was
 * applied, so a caller can tell "there are no more" from "you asked for fewer".
 * Returns how many were written to `out`.
 *
 * A copy rather than a borrow: the set is replaced wholesale by the next tick,
 * and handing out pointers into it would hand out pointers into freed memory
 * the moment the caller released the lock. */
size_t conflict_set_list(const ConflictSet *cs, const char *ns,
                         ConflictPair *out, size_t max, size_t *total);

#endif /* AEGISDB_CONFLICT_SET_H */
