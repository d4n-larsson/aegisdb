/* Query engine: operation router + core memory operations (T016). */
#ifndef AEGISDB_QUERY_ENGINE_H
#define AEGISDB_QUERY_ENGINE_H

#include "aegisdb/db.h"
#include "aegisdb/errors.h"
#include "aegisdb/record.h"
#include "cJSON.h"

/* Route a parsed request object to the matching operation and build a response
 * cJSON object (without request_id; the protocol layer adds it). Returns NULL
 * only on allocation failure. */
cJSON *query_engine_dispatch(AegisDB *db, const cJSON *req);

/* ----- core operations (exposed for unit testing) ----------------------- */

/* Insert. `in` carries type/data/tags/importance/confidence/embedding/agent_id
 * (ownership of its members stays with the caller). For working type pass a
 * session_id (and optional ttl_ms). On success *out holds the stored record
 * (caller record_free()s it). */
aegis_status_t qe_insert(AegisDB *db, const MemoryRecord *in,
                         const char *session_id, uint64_t ttl_ms,
                         MemoryRecord *out);

/* Get by id, optionally filtered by agent_id (NULL = no filter). */
aegis_status_t qe_get(AegisDB *db, uint64_t id, const char *agent_filter,
                      MemoryRecord *out);

/* Update a semantic record. Flags say which optional fields are present. */
typedef struct {
    int has_data;
    const void *data;
    size_t data_len;
    int has_importance;
    float importance;
    int has_confidence;
    float confidence;
    int has_tags;
    const char *const *tags;
    size_t tag_count;
} UpdatePatch;
/* Update a semantic record. When `ns` is non-NULL the record must belong to that
 * namespace, or it reads back as NOT_FOUND (no cross-tenant leak). */
aegis_status_t qe_update(AegisDB *db, uint64_t id, const UpdatePatch *patch,
                         const char *ns, MemoryRecord *out);

/* Soft-delete a persisted record by id (writes a tombstone version to the log
 * and drops it from the secondary indexes). Returns NOT_FOUND if the id is
 * unknown, already deleted, or (when `ns` is non-NULL) outside that namespace.
 * Works for both episodic and semantic records. */
aegis_status_t qe_delete(AegisDB *db, uint64_t id, const char *ns);

typedef struct {
    int has_time;
    uint64_t start_time, end_time;
    const char *const *tags;
    size_t tag_count;
    int match_all;
    const float *embedding;
    size_t embedding_dim;
    int has_type;
    MemoryType type;
    const char *agent_id;
    size_t top_k;
    size_t offset;      /* skip this many top-ranked results (pagination) */
    int has_min_score;  /* semantic only: drop matches below min_score */
    float min_score;    /* cosine-similarity floor in [-1, 1] */
    uint64_t half_life_ms; /* semantic only: recency half-life; 0 = no decay */
    int has_max_importance; /* filter: keep only records with importance <= max */
    float max_importance;
    int oldest_first;   /* non-semantic: when a bounded time scan truncates, keep
                         * the OLDEST rather than the most recent (candidate
                         * selection for summarization). Ignored for semantic. */
} SearchParams;

/* Per-hit ranking explanation (ROADMAP 1.2), one entry parallel to each returned
 * record, in the same order. Explains why a hit ranked where it did:
 *   score = weight * similarity * recency_factor   (semantic queries)
 * so a client/operator can see the contribution of each factor. */
typedef struct {
    int semantic;         /* 1 if ranked by semantic similarity, 0 otherwise */
    float similarity;     /* raw cosine similarity [-1,1]; 0 for non-semantic */
    float importance;     /* record importance */
    float confidence;     /* record confidence */
    float weight;         /* importance*confidence applied (1.0 if that was <=0) */
    float recency_factor; /* recency-decay multiplier in (0,1]; 1.0 if no half-life */
    float score;          /* final rank score */
} SearchExplain;

/* Search. Allocates *out_records (array of MemoryRecord; record_free each then
 * free the array). */
aegis_status_t qe_search(AegisDB *db, const SearchParams *p,
                         MemoryRecord **out_records, size_t *out_n);

/* Like qe_search, but when out_explain is non-NULL also allocates a parallel
 * array of SearchExplain (one per returned record, same order; free it with a
 * single free()). Pass NULL to behave exactly like qe_search. ROADMAP 1.2. */
aegis_status_t qe_search_ex(AegisDB *db, const SearchParams *p,
                            MemoryRecord **out_records,
                            SearchExplain **out_explain, size_t *out_n);

/* Count live records matching the filters (type/tags/time/agent_id); ignores
 * any embedding. Returns the count in *out_count. A broad/filterless count is
 * bounded by the query scan cap; when that cap truncates the scan the count is
 * a floor and *out_capped (may be NULL) is set to 1. */
aegis_status_t qe_count(AegisDB *db, const SearchParams *p, size_t *out_count,
                        int *out_capped);

/* Delete all live records matching the filters, scoped to `ns` when non-NULL.
 * Requires at least one positive filter (type/tags/time) — refuses an
 * unfiltered bulk delete with INVALID_REQUEST. Returns the count in
 * *out_deleted. */
/* Export a subject's records (ROADMAP 3.2): live records owned by `ns`, id-order,
 * with id > after_id, up to `limit`. Allocates *out_records (record_free each,
 * then free the array); *out_has_more (may be NULL) signals another page. Used
 * for compliance "export what you store about me". */
aegis_status_t qe_export(AegisDB *db, const char *ns, uint64_t after_id,
                         size_t limit, MemoryRecord **out_records, size_t *out_n,
                         int *out_has_more);

/* Right-to-be-forgotten (ROADMAP 3.2): tombstone every record owned by `ns`
 * (which must be non-empty — a global purge is refused). The caller then runs
 * compaction so the payloads leave the on-disk log. `dry_run` counts without
 * deleting. Reports the count in *out_count. */
aegis_status_t qe_purge_namespace(AegisDB *db, const char *ns, int dry_run,
                                  size_t *out_count);

aegis_status_t qe_delete_by_query(AegisDB *db, const SearchParams *p,
                                  const char *ns, size_t *out_deleted);

/* Tombstone every live record whose TTL horizon has passed (expires_at != 0 &&
 * now >= expires_at). Called by the maintenance thread; returns the count
 * swept. */
size_t qe_sweep_expired(AegisDB *db, uint64_t now);

/* Merge near-duplicate semantic records (cosine >= min_similarity) within `ns`
 * (all tenants when NULL): each cluster collapses to its most-recently-updated
 * member, which absorbs the others' tags/relationships and the max
 * importance/confidence; the rest are tombstoned. Reports the cluster and
 * merged-away counts. Idempotent. */
aegis_status_t qe_consolidate(AegisDB *db, const char *ns, float min_similarity,
                              size_t *out_clusters, size_t *out_merged);

/* Decay-based forgetting (ROADMAP 2.3): tombstone aging, low-value records so the
 * corpus and its in-RAM indexes plateau instead of growing without bound.
 * Retention = importance * 0.5^(age/half_life) (age from `updated`); a record is
 * forgotten when retention < min_retention. Scoped to one `type` (episodic is the
 * intended default — curated semantic facts should be protected) and to `ns`.
 * `dry_run` counts what would be forgotten without deleting; `max_forget` (0 =
 * unbounded) caps deletions. Reports *out_scanned (examined) and *out_forgotten. */
aegis_status_t qe_forget(AegisDB *db, const char *ns, MemoryType type,
                         uint64_t half_life_ms, float min_retention, int dry_run,
                         size_t max_forget, size_t *out_scanned,
                         size_t *out_forgotten);

/* Promote a working record to a persisted one. When `ns` is non-NULL the new
 * record is pinned to that namespace (agent_id). */
aegis_status_t qe_promote(AegisDB *db, const char *session_id,
                          uint64_t working_id, MemoryType to_type,
                          const char *ns, MemoryRecord *out);

/* Add a directed relationship from_id -> to_id. When `ns` is non-NULL both
 * endpoints must belong to that namespace, or NOT_FOUND (no cross-tenant leak). */
aegis_status_t qe_relate(AegisDB *db, uint64_t from_id, uint64_t to_id,
                         const char *kind, const char *ns);

/* Breadth-first relationship traversal from start_id up to `depth` hops. */
aegis_status_t qe_traverse(AegisDB *db, uint64_t start_id, int depth,
                           const char *agent_filter, MemoryRecord **out,
                           size_t *out_n);

#endif /* AEGISDB_QUERY_ENGINE_H */