/* Reverse relationship index (ROADMAP 5.1): to_id -> the sources pointing at it.
 *
 * Relationships are stored *inside* the record (see record.h), which makes the
 * record its own forward adjacency list: a traversal already decodes each
 * frontier record, so following edges outward needs no index at all. The
 * reverse direction has no such luck — answering "what supersedes this?" or
 * "what was derived from this?" without an index means decoding every live
 * record in the corpus. This index supplies that direction, and only that
 * direction; the asymmetry is deliberate, not an omission.
 *
 * It is in-RAM and *derived*: nothing is persisted, and recovery rebuilds it
 * from the log exactly as it rebuilds the tag, time, and lexical indexes. Keyed
 * on ids rather than log offsets, so compaction's log rewrite is a non-event.
 * Not internally synchronized — callers hold db->index_lock, as they do for the
 * tag and lexical indexes.
 */
#ifndef AEGISDB_EDGE_INDEX_H
#define AEGISDB_EDGE_INDEX_H

#include <stddef.h>
#include <stdint.h>

/* Distinct edge kinds that can be interned. The kind vocabulary is
 * low-cardinality by nature (`supersedes`, `derived_from`, …), but `kind` is a
 * client-supplied string with no cardinality limit of its own, so the table is
 * capped rather than trusted. Past the cap an edge is still indexed — its kind
 * is simply recorded as unknown (see EdgeSource.kind_unknown), which costs
 * filter precision, never completeness. */
#define EDGE_MAX_KINDS 4096

/* Longest internable kind. A longer one is treated as un-internable (indexed
 * with kind_unknown set) rather than truncated: truncating would silently
 * collapse two distinct kinds into one and make the filter answer wrongly,
 * which is worse than answering imprecisely and saying so. */
#define EDGE_MAX_KIND_LEN 64

typedef struct EdgeIndex EdgeIndex;

/* One source pointing at the queried record. */
typedef struct {
    uint64_t from_id;
    /* The edge's kind: an interned string, or NULL for an edge stored without
     * one (`relate` permits that). Interned strings are allocated once and never
     * moved or freed while the index lives, so this pointer stays valid for the
     * index's lifetime — a caller may hold it past the lock it queried under. */
    const char *kind;
    /* 1 when this edge's kind could not be interned (the table was full, or the
     * kind was longer than EDGE_MAX_KIND_LEN), so `kind` is *unknown* rather
     * than absent. Such an edge is returned by every kind filter as a
     * candidate; a caller that needs precision must confirm against the source
     * record, which it is generally about to read anyway. Always 0 until a
     * corpus actually exceeds the cap. */
    int kind_unknown;
} EdgeSource;

EdgeIndex *edge_index_create(void);
void edge_index_free(EdgeIndex *e);

/* Record that from_id -> to_id (kind may be NULL). Re-adding an identical
 * (from_id, kind) edge for the same target is a no-op returning 0, matching
 * `relate`'s own idempotency. Returns 0 on success (including the no-op), -1 on
 * allocation failure.
 *
 * Every entry point tolerates a NULL index and treats it as "no edge index
 * configured" (--no-edge-index), so write-path call sites can call these
 * unconditionally rather than guarding each one. */
int edge_index_add(EdgeIndex *e, uint64_t from_id, uint64_t to_id,
                   const char *kind);

/* Drop one edge. `kind` must be the kind it was added with. Absent edges are
 * ignored. */
void edge_index_remove(EdgeIndex *e, uint64_t from_id, uint64_t to_id,
                       const char *kind);

/* Drop every edge pointing *at* `id`, in O(indegree) — no corpus scan. This is
 * what a tombstone needs: the record being deleted knows its own outgoing edges
 * (they are in its relationships array) but not its incoming ones. */
void edge_index_remove_target(EdgeIndex *e, uint64_t id);

/* Sources pointing at `to_id`, optionally restricted to `kinds` (an empty
 * filter matches every kind). Allocates *out (free with free()); *out_n is 0
 * with *out NULL when nothing matches. Returns 0/-1.
 *
 * Results are sorted by from_id ascending — that much is contractual, and is
 * what makes paging stable. Among edges *sharing* a from_id the order follows an
 * internal kind id assigned in first-seen order: deterministic for a given
 * index, but neither lexicographic nor predictable from the outside. Do not
 * depend on it; sort the (small) result if you need a particular order.
 *
 * A kind filter also admits every edge whose kind is unknown — see
 * EdgeSource.kind_unknown. */
int edge_index_sources(const EdgeIndex *e, uint64_t to_id,
                       const char *const *kinds, size_t n_kinds,
                       EdgeSource **out, size_t *out_n);

/* Edges currently indexed. */
size_t edge_index_edges(const EdgeIndex *e);
/* Distinct kinds currently carrying at least one edge — not kinds ever seen, so
 * the number is stable across a restart that replays the same log. (The intern
 * table behind it never shrinks: a kind's string stays allocated so ids stay
 * stable and a returned `kind` pointer stays valid, which is why EDGE_MAX_KINDS
 * bounds distinct kinds *ever interned* rather than the count reported here.) */
size_t edge_index_kinds(const EdgeIndex *e);
/* Approximate resident bytes (target table + posting arrays + intern table).
 * Excludes allocator overhead. */
size_t edge_index_bytes(const EdgeIndex *e);

#endif /* AEGISDB_EDGE_INDEX_H */
