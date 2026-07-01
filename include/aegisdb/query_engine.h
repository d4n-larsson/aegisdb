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
} SearchParams;

/* Search. Allocates *out_records (array of MemoryRecord; record_free each then
 * free the array). */
aegis_status_t qe_search(AegisDB *db, const SearchParams *p,
                         MemoryRecord **out_records, size_t *out_n);

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