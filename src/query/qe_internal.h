/* Internal contract shared across the query-engine translation units
 * (query_engine.c core/graph, qe_search.c, qe_maint.c, qe_dispatch.c). Not a
 * public header — the public API is include/aegisdb/query_engine.h. Holds the
 * engine-wide limits and the one core helper called from more than one TU. */
#ifndef AEGISDB_QE_INTERNAL_H
#define AEGISDB_QE_INTERNAL_H

#include "aegisdb/db.h"
#include "aegisdb/errors.h"

#define MAX_TAGS 32     /* max tags per record (also enforced in validate_common) */
#define MAX_RELATIONSHIPS 4096 /* max relationships per record. Kept well below
                                * UINT16_MAX so the u16 wire count in record_encode
                                * can never truncate (which would render the record
                                * undecodable = durable data loss). */
#define MAX_TOP_K 1000  /* clamp untrusted top_k to bound work/allocations */
#define SEARCH_FETCH_CAP 8192 /* upper bound on semantic over-fetch when widening
                               * to satisfy a selective filter (bounds worst-case
                               * work; a very selective filter may still yield
                               * fewer than top_k — inherent to filtered ANN) */
#define MAX_OFFSET 100000 /* clamp pagination offset to bound ranking work/allocs */
#define MAX_VECS_PER_RECORD 64 /* cap embeddings per record (#85) */
#define MAX_TRAVERSE_DEPTH 64 /* clamp graph-traversal depth (bounds work + the int cast) */
#define MIN_HALF_LIFE_MS 1000 /* floor recency half-life at 1s (avoid absurd decay) */
#define MAX_AGENT_ID 128

/* Phase gating: fail with NOT_READY when the server runs below the phase a
 * feature needs. Defined in query_engine.c; used by every engine TU. */
aegis_status_t require_phase(const AegisDB *db, int needed);

/* Cross-tenant guard: true when `ns` is set and does not own record `r` (its
 * agent_id differs or is absent) — such a record must read as missing. Defined
 * in query_engine.c. */
int ns_denies(const char *ns, const MemoryRecord *r);

/* True when a TTL'd record is past its expiry horizon `now` (ms) — archived:
 * hidden from recall until the sweep tombstones it. Defined in query_engine.c. */
int record_expired(const MemoryRecord *r, uint64_t now);

/* A scored candidate record. Produced/ranked in qe_search.c; also used as a
 * (record, score) accumulator by the graph traversal in query_engine.c. The
 * ranking-breakdown fields are only meaningful for semantic search. */
typedef struct {
    MemoryRecord rec;
    float score;
    float sim;     /* raw cosine similarity [-1,1] */
    float weight;  /* importance*confidence actually applied (1.0 if that was <=0) */
    float recency; /* recency-decay multiplier in (0,1]; 1.0 when no half-life */
} Cand;

#endif /* AEGISDB_QE_INTERNAL_H */