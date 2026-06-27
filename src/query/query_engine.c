/* Query engine: operation router + core memory operations.
 *
 * T016 router; T020-T022/T026 (US1 insert/get/ping/errors); T029-T031 (US2
 * semantic insert/update + time/tag search); T037-T038 (US3 semantic search +
 * re-ranking); T043-T044 (US4 working insert/promote); T047-T049 (US5 relate/
 * traverse/agent scoping); T055 (phase gating). */
#include "aegisdb/query_engine.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "aegisdb/json_request.h"
#include "aegisdb/json_response.h"

/* ----- phase gating (T055) --------------------------------------------- */

static aegis_status_t require_phase(const AegisDB *db, int needed) {
    return (db->config.enabled_phase >= needed) ? AEGIS_OK
                                                : AEGIS_ERR_NOT_READY;
}

/* ----- validation ------------------------------------------------------- */

static int valid_tag(const char *t) {
    size_t n = strlen(t);
    if (n < 1 || n > 64) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static aegis_status_t validate_common(AegisDB *db, const MemoryRecord *r) {
    if (r->data_len > db->config.max_payload_bytes)
        return AEGIS_ERR_PAYLOAD_TOO_LARGE;
    if (r->importance < 0.0f || r->importance > 1.0f)
        return AEGIS_ERR_INVALID_REQUEST;
    if (r->confidence < 0.0f || r->confidence > 1.0f)
        return AEGIS_ERR_INVALID_REQUEST;
    if (r->tag_count > 32) return AEGIS_ERR_INVALID_REQUEST;
    for (size_t i = 0; i < r->tag_count; i++)
        if (!valid_tag(r->tags[i])) return AEGIS_ERR_INVALID_REQUEST;
    if (r->embedding_dim &&
        r->embedding_dim != db->config.embedding_dimensions)
        return AEGIS_ERR_INVALID_REQUEST;
    return AEGIS_OK;
}

/* ----- low-level persistence (caller holds index write lock) ------------ */

static aegis_status_t append_and_hash(AegisDB *db, const MemoryRecord *rec) {
    uint8_t *buf = NULL;
    size_t len = 0;
    if (record_encode(rec, &buf, &len) != 0) return AEGIS_ERR_INTERNAL;
    uint64_t off = 0;
    int rv = log_append(&db->log, buf, len, &off);
    free(buf);
    if (rv != 0) return AEGIS_ERR_INTERNAL;
    if (hash_index_put(db->hash, rec->id, off, (uint32_t)len, (uint8_t)rec->type,
                       (uint8_t)(rec->deleted ? 1 : 0)) != 0)
        return AEGIS_ERR_INTERNAL;
    return AEGIS_OK;
}

/* Read + decode a live persisted record (caller holds at least a read lock). */
static aegis_status_t load_record(AegisDB *db, uint64_t id, MemoryRecord *out) {
    const HashEntry *e = hash_index_get(db->hash, id);
    if (!e) return AEGIS_ERR_NOT_FOUND;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (log_read(&db->log, e->offset, &buf, &len) != 0)
        return AEGIS_ERR_INTERNAL;
    int rv = record_decode(buf, len, out);
    free(buf);
    return rv == 0 ? AEGIS_OK : AEGIS_ERR_INTERNAL;
}

/* ----- core operations -------------------------------------------------- */

aegis_status_t qe_insert(AegisDB *db, const MemoryRecord *in,
                         const char *session_id, uint64_t ttl_ms,
                         MemoryRecord *out) {
    aegis_status_t st = validate_common(db, in);
    if (st != AEGIS_OK) return st;
    if (in->data_len == 0) return AEGIS_ERR_INVALID_REQUEST;

    if (in->type == MEM_WORKING) {
        st = require_phase(db, 4);
        if (st != AEGIS_OK) return st;
        if (!session_id) return AEGIS_ERR_INVALID_REQUEST;
        uint64_t now = db_now_ms();
        uint64_t wid = 0;
        if (working_store_add(db->working, session_id, in, now, ttl_ms, &wid) !=
            0)
            return AEGIS_ERR_INTERNAL;
        MemoryRecord *got = working_store_get(db->working, session_id, wid, now);
        if (!got) return AEGIS_ERR_INTERNAL;
        *out = *got;
        free(got);
        return AEGIS_OK;
    }

    if (in->type == MEM_EPISODIC) {
        st = require_phase(db, 1);
    } else if (in->type == MEM_SEMANTIC) {
        st = require_phase(db, 2);
    } else {
        return AEGIS_ERR_INVALID_REQUEST;
    }
    if (st != AEGIS_OK) return st;

    MemoryRecord *rec = record_clone(in);
    if (!rec) return AEGIS_ERR_INTERNAL;
    uint64_t now = db_now_ms();
    rec->id = db_next_id(db);
    rec->created = now;
    rec->updated = now; /* episodic: updated == created */
    rec->deleted = 0;
    rec->expires_at = 0;

    pthread_rwlock_wrlock(&db->index_lock);
    st = append_and_hash(db, rec);
    if (st == AEGIS_OK) {
        time_index_add(db->time, rec->created, rec->id);
        for (size_t i = 0; i < rec->tag_count; i++)
            tag_index_add(db->tags, rec->tags[i], rec->id);
        if (rec->embedding_dim)
            semantic_index_add(db->sem, rec->id, rec->embedding,
                               rec->embedding_dim);
    }
    pthread_rwlock_unlock(&db->index_lock);

    if (st != AEGIS_OK) {
        record_free(rec);
        free(rec);
        return st;
    }
    *out = *rec; /* move ownership */
    free(rec);
    return AEGIS_OK;
}

aegis_status_t qe_get(AegisDB *db, uint64_t id, const char *agent_filter,
                      MemoryRecord *out) {
    pthread_rwlock_rdlock(&db->index_lock);
    aegis_status_t st = load_record(db, id, out);
    pthread_rwlock_unlock(&db->index_lock);
    if (st != AEGIS_OK) return st;
    if (out->deleted) {
        record_free(out);
        return AEGIS_ERR_NOT_FOUND;
    }
    if (agent_filter && (!out->agent_id ||
                         strcmp(out->agent_id, agent_filter) != 0)) {
        record_free(out);
        return AEGIS_ERR_NOT_FOUND;
    }
    return AEGIS_OK;
}

aegis_status_t qe_update(AegisDB *db, uint64_t id, const UpdatePatch *patch,
                         MemoryRecord *out) {
    aegis_status_t st = require_phase(db, 2);
    if (st != AEGIS_OK) return st;

    pthread_rwlock_wrlock(&db->index_lock);
    MemoryRecord cur;
    st = load_record(db, id, &cur);
    if (st != AEGIS_OK) {
        pthread_rwlock_unlock(&db->index_lock);
        return st;
    }
    if (cur.deleted) {
        record_free(&cur);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_NOT_FOUND;
    }
    if (cur.type != MEM_SEMANTIC) {
        record_free(&cur);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_IMMUTABLE;
    }

    /* old tags for index diff */
    if (patch->has_data) {
        void *nd = NULL;
        if (patch->data_len) {
            nd = malloc(patch->data_len);
            if (!nd) {
                record_free(&cur);
                pthread_rwlock_unlock(&db->index_lock);
                return AEGIS_ERR_INTERNAL;
            }
            memcpy(nd, patch->data, patch->data_len);
        }
        free(cur.data);
        cur.data = nd;
        cur.data_len = patch->data_len;
    }
    if (patch->has_importance) cur.importance = patch->importance;
    if (patch->has_confidence) cur.confidence = patch->confidence;

    if (patch->has_tags) {
        for (size_t i = 0; i < cur.tag_count; i++)
            tag_index_remove(db->tags, cur.tags[i], cur.id);
        if (record_set_tags(&cur, patch->tags, patch->tag_count) != 0) {
            record_free(&cur);
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
        for (size_t i = 0; i < cur.tag_count; i++)
            tag_index_add(db->tags, cur.tags[i], cur.id);
    }

    cur.updated = db_now_ms();
    st = validate_common(db, &cur);
    if (st == AEGIS_OK) st = append_and_hash(db, &cur);
    pthread_rwlock_unlock(&db->index_lock);

    if (st != AEGIS_OK) {
        record_free(&cur);
        return st;
    }
    *out = cur; /* move ownership */
    return AEGIS_OK;
}

aegis_status_t qe_delete(AegisDB *db, uint64_t id) {
    aegis_status_t st = require_phase(db, 1);
    if (st != AEGIS_OK) return st;

    pthread_rwlock_wrlock(&db->index_lock);
    MemoryRecord cur;
    st = load_record(db, id, &cur);
    if (st != AEGIS_OK) {
        pthread_rwlock_unlock(&db->index_lock);
        return st;
    }
    if (cur.deleted) {
        record_free(&cur);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_NOT_FOUND;
    }

    /* drop from secondary indexes so it stops surfacing in queries */
    for (size_t i = 0; i < cur.tag_count; i++)
        tag_index_remove(db->tags, cur.tags[i], cur.id);
    if (cur.embedding_dim)
        semantic_index_remove(db->sem, cur.id);
    time_index_remove(db->time, cur.created, cur.id);

    cur.deleted = 1;
    cur.updated = db_now_ms();
    st = append_and_hash(db, &cur); /* tombstone version; hash marks deleted */
    pthread_rwlock_unlock(&db->index_lock);

    record_free(&cur);
    return st;
}

/* predicate helpers for search */
static int rec_has_tag(const MemoryRecord *r, const char *tag) {
    for (size_t i = 0; i < r->tag_count; i++)
        if (strcmp(r->tags[i], tag) == 0) return 1;
    return 0;
}

static int passes_filters(const MemoryRecord *r, const SearchParams *p) {
    if (r->deleted) return 0;
    if (p->has_type && r->type != p->type) return 0;
    if (p->agent_id &&
        (!r->agent_id || strcmp(r->agent_id, p->agent_id) != 0))
        return 0;
    if (p->has_time && (r->created < p->start_time || r->created > p->end_time))
        return 0;
    if (p->tag_count) {
        if (p->match_all) {
            for (size_t i = 0; i < p->tag_count; i++)
                if (!rec_has_tag(r, p->tags[i])) return 0;
        } else {
            int any = 0;
            for (size_t i = 0; i < p->tag_count; i++)
                if (rec_has_tag(r, p->tags[i])) { any = 1; break; }
            if (!any) return 0;
        }
    }
    return 1;
}

typedef struct {
    MemoryRecord rec;
    float score;
} Cand;

static int cmp_score_desc(const void *a, const void *b) {
    float x = ((const Cand *)a)->score, y = ((const Cand *)b)->score;
    if (x < y) return 1;
    if (x > y) return -1;
    return 0;
}
static int cmp_created_asc(const void *a, const void *b) {
    uint64_t x = ((const Cand *)a)->rec.created, y = ((const Cand *)b)->rec.created;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

aegis_status_t qe_search(AegisDB *db, const SearchParams *p,
                         MemoryRecord **out_records, size_t *out_n) {
    aegis_status_t st = require_phase(db, p->embedding_dim ? 3 : 2);
    if (st != AEGIS_OK) return st;
    if (p->embedding_dim &&
        p->embedding_dim != db->config.embedding_dimensions)
        return AEGIS_ERR_INVALID_REQUEST;

    size_t top_k = p->top_k ? p->top_k : 10;

    pthread_rwlock_rdlock(&db->index_lock);

    /* 1. candidate id set + optional similarity scores */
    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t nids = 0;
    int semantic = 0;

    if (p->embedding_dim) {
        semantic = 1;
        /* over-fetch so post-filters still leave enough results */
        size_t fetch = top_k * 4 + 32;
        if (semantic_index_search(db->sem, p->embedding, p->embedding_dim, fetch,
                                  &ids, &scores, &nids) != 0) {
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
    } else if (p->has_time) {
        if (time_index_range(db->time, p->start_time, p->end_time, 0, &ids,
                             &nids) != 0) {
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
    } else if (p->tag_count) {
        if (tag_index_query(db->tags, p->tags, p->tag_count, p->match_all, &ids,
                            &nids) != 0) {
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
    } else {
        /* no positive filter: scan all live records chronologically */
        if (time_index_range(db->time, 0, UINT64_MAX, 0, &ids, &nids) != 0) {
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
    }

    /* 2. load + post-filter */
    Cand *cands = malloc((nids ? nids : 1) * sizeof(Cand));
    if (!cands) {
        free(ids);
        free(scores);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_INTERNAL;
    }
    size_t m = 0;
    for (size_t i = 0; i < nids; i++) {
        MemoryRecord r;
        if (load_record(db, ids[i], &r) != AEGIS_OK) continue;
        if (!passes_filters(&r, p)) {
            record_free(&r);
            continue;
        }
        cands[m].rec = r;
        if (semantic) {
            /* T038: re-rank by importance * confidence * similarity */
            float sim = scores[i];
            float w = r.importance * r.confidence;
            cands[m].score = (w > 0 ? w : 1.0f) * sim;
        } else {
            cands[m].score = 0;
        }
        m++;
    }
    pthread_rwlock_unlock(&db->index_lock);
    free(ids);
    free(scores);

    /* 3. order + truncate */
    if (semantic)
        qsort(cands, m, sizeof(Cand), cmp_score_desc);
    else
        qsort(cands, m, sizeof(Cand), cmp_created_asc);

    size_t keep = (top_k < m) ? top_k : m;
    MemoryRecord *res = malloc((keep ? keep : 1) * sizeof(MemoryRecord));
    if (!res) {
        for (size_t i = 0; i < m; i++) record_free(&cands[i].rec);
        free(cands);
        return AEGIS_ERR_INTERNAL;
    }
    for (size_t i = 0; i < keep; i++) res[i] = cands[i].rec;
    for (size_t i = keep; i < m; i++) record_free(&cands[i].rec);
    free(cands);
    *out_records = res;
    *out_n = keep;
    return AEGIS_OK;
}

aegis_status_t qe_promote(AegisDB *db, const char *session_id,
                          uint64_t working_id, MemoryType to_type,
                          MemoryRecord *out) {
    aegis_status_t st = require_phase(db, 4);
    if (st != AEGIS_OK) return st;
    if (!session_id) return AEGIS_ERR_INVALID_REQUEST;
    if (to_type != MEM_EPISODIC && to_type != MEM_SEMANTIC)
        return AEGIS_ERR_INVALID_REQUEST;

    MemoryRecord w;
    if (working_store_take(db->working, session_id, working_id, db_now_ms(),
                           &w) != 0)
        return AEGIS_ERR_NOT_FOUND;

    /* re-insert as a persisted record */
    w.type = to_type;
    w.expires_at = 0;
    st = qe_insert(db, &w, NULL, 0, out);
    record_free(&w);
    return st;
}

aegis_status_t qe_relate(AegisDB *db, uint64_t from_id, uint64_t to_id,
                         const char *kind) {
    aegis_status_t st = require_phase(db, 4);
    if (st != AEGIS_OK) return st;

    pthread_rwlock_wrlock(&db->index_lock);
    /* target must exist */
    if (!hash_index_get(db->hash, to_id)) {
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_NOT_FOUND;
    }
    MemoryRecord from;
    st = load_record(db, from_id, &from);
    if (st != AEGIS_OK || from.deleted) {
        if (st == AEGIS_OK) record_free(&from);
        pthread_rwlock_unlock(&db->index_lock);
        return st == AEGIS_OK ? AEGIS_ERR_NOT_FOUND : st;
    }
    if (record_add_relationship(&from, from_id, to_id, kind) != 0) {
        record_free(&from);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_INTERNAL;
    }
    from.updated = db_now_ms();
    st = append_and_hash(db, &from); /* relationship metadata, content intact */
    pthread_rwlock_unlock(&db->index_lock);
    record_free(&from);
    return st;
}

aegis_status_t qe_traverse(AegisDB *db, uint64_t start_id, int depth,
                           const char *agent_filter, MemoryRecord **out,
                           size_t *out_n) {
    aegis_status_t st = require_phase(db, 4);
    if (st != AEGIS_OK) return st;
    if (depth < 0) depth = 0;

    /* BFS over relationship edges */
    uint64_t *seen = NULL;
    size_t seen_n = 0, seen_cap = 0;
    uint64_t *frontier = malloc(sizeof(uint64_t));
    size_t front_n = 0;
    if (!frontier) return AEGIS_ERR_INTERNAL;
    frontier[front_n++] = start_id;

    Cand *acc = NULL;
    size_t acc_n = 0, acc_cap = 0;

    pthread_rwlock_rdlock(&db->index_lock);
    for (int level = 0; level <= depth && front_n > 0; level++) {
        uint64_t *next = NULL;
        size_t next_n = 0, next_cap = 0;
        for (size_t i = 0; i < front_n; i++) {
            uint64_t id = frontier[i];
            int dup = 0;
            for (size_t s = 0; s < seen_n; s++)
                if (seen[s] == id) { dup = 1; break; }
            if (dup) continue;
            if (seen_n == seen_cap) {
                seen_cap = seen_cap ? seen_cap * 2 : 8;
                seen = realloc(seen, seen_cap * sizeof(uint64_t));
            }
            seen[seen_n++] = id;

            MemoryRecord r;
            if (load_record(db, id, &r) != AEGIS_OK) continue;
            if (r.deleted ||
                (agent_filter && (!r.agent_id ||
                                  strcmp(r.agent_id, agent_filter) != 0))) {
                /* still traverse edges of filtered node? skip it entirely */
                record_free(&r);
                continue;
            }
            /* collect */
            if (acc_n == acc_cap) {
                acc_cap = acc_cap ? acc_cap * 2 : 8;
                acc = realloc(acc, acc_cap * sizeof(Cand));
            }
            acc[acc_n].rec = r; /* keep; do not free */
            acc[acc_n].score = 0;
            acc_n++;
            /* enqueue neighbours */
            for (size_t k = 0; k < r.rel_count; k++) {
                if (next_n == next_cap) {
                    next_cap = next_cap ? next_cap * 2 : 8;
                    next = realloc(next, next_cap * sizeof(uint64_t));
                }
                next[next_n++] = r.relationships[k].to_id;
            }
        }
        free(frontier);
        frontier = next;
        front_n = next_n;
        (void)next_cap;
    }
    pthread_rwlock_unlock(&db->index_lock);
    free(frontier);
    free(seen);

    MemoryRecord *res = malloc((acc_n ? acc_n : 1) * sizeof(MemoryRecord));
    if (!res) {
        for (size_t i = 0; i < acc_n; i++) record_free(&acc[i].rec);
        free(acc);
        return AEGIS_ERR_INTERNAL;
    }
    for (size_t i = 0; i < acc_n; i++) res[i] = acc[i].rec;
    free(acc);
    *out = res;
    *out_n = acc_n;
    return AEGIS_OK;
}

/* ----- dispatch / wire handlers ---------------------------------------- */

static cJSON *resp_record(const MemoryRecord *r) {
    cJSON *o = json_ok();
    if (!o) return NULL;
    cJSON_AddItemToObject(o, "record", json_record(r));
    return o;
}

static int build_input_record(AegisDB *db, const cJSON *req, MemoryRecord *in,
                              aegis_status_t *err) {
    record_init(in);
    const char *type = jr_str(req, "type", NULL);
    if (!type || memory_type_from_string(type, &in->type) != 0) {
        *err = AEGIS_ERR_INVALID_REQUEST;
        return -1;
    }
    const char *data = jr_str(req, "data", NULL);
    if (!data) {
        *err = AEGIS_ERR_INVALID_REQUEST;
        return -1;
    }
    in->data_len = strlen(data);
    in->data = malloc(in->data_len ? in->data_len : 1);
    if (!in->data) {
        *err = AEGIS_ERR_INTERNAL;
        return -1;
    }
    memcpy(in->data, data, in->data_len);

    double d;
    if (jr_f64(req, "importance", &d) == 0) in->importance = (float)d;
    if (jr_f64(req, "confidence", &d) == 0) in->confidence = (float)d;
    const char *agent = jr_str(req, "agent_id", NULL);
    if (agent) {
        in->agent_id = strdup(agent);
        if (!in->agent_id) { *err = AEGIS_ERR_INTERNAL; return -1; }
    }
    const char **tags = NULL;
    size_t tn = 0;
    jr_str_array(req, "tags", &tags, &tn);
    if (tn && record_set_tags(in, tags, tn) != 0) {
        free(tags);
        *err = AEGIS_ERR_INTERNAL;
        return -1;
    }
    free(tags);
    float *emb = NULL;
    size_t en = 0;
    jr_float_array(req, "embedding", &emb, &en);
    if (en) {
        in->embedding = emb;
        in->embedding_dim = en;
    }
    *err = AEGIS_OK;
    return 0;
}

static cJSON *handle_ping(AegisDB *db) {
    cJSON *o = json_ok();
    cJSON_AddStringToObject(o, "version", AEGIS_VERSION_STRING);
    cJSON_AddNumberToObject(o, "phase", db->config.enabled_phase);
    return o;
}

static cJSON *handle_insert(AegisDB *db, const cJSON *req) {
    MemoryRecord in;
    aegis_status_t err;
    if (build_input_record(db, req, &in, &err) != 0) {
        record_free(&in);
        return json_error_status(err);
    }
    const char *session = jr_str(req, "session_id", NULL);
    uint64_t ttl = 0;
    jr_u64(req, "ttl_ms", &ttl);
    MemoryRecord out;
    aegis_status_t st = qe_insert(db, &in, session, ttl, &out);
    record_free(&in);
    if (st != AEGIS_OK) return json_error_status(st);
    cJSON *r = resp_record(&out);
    record_free(&out);
    return r;
}

static cJSON *handle_get(AegisDB *db, const cJSON *req) {
    uint64_t id;
    if (jr_u64(req, "id", &id) != 0) return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    const char *agent = jr_str(req, "agent_id", NULL);
    MemoryRecord out;
    aegis_status_t st = qe_get(db, id, agent, &out);
    if (st != AEGIS_OK) return json_error_status(st);
    cJSON *r = resp_record(&out);
    record_free(&out);
    return r;
}

static cJSON *handle_update(AegisDB *db, const cJSON *req) {
    uint64_t id;
    if (jr_u64(req, "id", &id) != 0) return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    UpdatePatch patch;
    memset(&patch, 0, sizeof(patch));
    const char *data = jr_str(req, "data", NULL);
    if (data) {
        patch.has_data = 1;
        patch.data = data;
        patch.data_len = strlen(data);
    }
    double d;
    if (jr_f64(req, "importance", &d) == 0) {
        patch.has_importance = 1;
        patch.importance = (float)d;
    }
    if (jr_f64(req, "confidence", &d) == 0) {
        patch.has_confidence = 1;
        patch.confidence = (float)d;
    }
    const char **tags = NULL;
    size_t tn = 0;
    if (cJSON_GetObjectItemCaseSensitive(req, "tags")) {
        jr_str_array(req, "tags", &tags, &tn);
        patch.has_tags = 1;
        patch.tags = tags;
        patch.tag_count = tn;
    }
    MemoryRecord out;
    aegis_status_t st = qe_update(db, id, &patch, &out);
    free(tags);
    if (st != AEGIS_OK) return json_error_status(st);
    cJSON *r = resp_record(&out);
    record_free(&out);
    return r;
}

static cJSON *handle_delete(AegisDB *db, const cJSON *req) {
    uint64_t id;
    if (jr_u64(req, "id", &id) != 0)
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    aegis_status_t st = qe_delete(db, id);
    if (st != AEGIS_OK) return json_error_status(st);
    cJSON *o = json_ok();
    cJSON_AddNumberToObject(o, "id", (double)id);
    cJSON_AddBoolToObject(o, "deleted", 1);
    return o;
}

static cJSON *handle_search(AegisDB *db, const cJSON *req) {
    SearchParams p;
    memset(&p, 0, sizeof(p));
    uint64_t v;
    if (jr_u64(req, "start_time", &p.start_time) == 0 &&
        jr_u64(req, "end_time", &p.end_time) == 0)
        p.has_time = 1;
    const char *type = jr_str(req, "type", NULL);
    if (type && memory_type_from_string(type, &p.type) == 0) p.has_type = 1;
    p.agent_id = jr_str(req, "agent_id", NULL);
    const char *match = jr_str(req, "match", "all");
    p.match_all = (strcmp(match, "any") != 0);
    if (jr_u64(req, "top_k", &v) == 0) p.top_k = (size_t)v;

    const char **tags = NULL;
    size_t tn = 0;
    jr_str_array(req, "tags", &tags, &tn);
    p.tags = tags;
    p.tag_count = tn;
    float *emb = NULL;
    size_t en = 0;
    jr_float_array(req, "embedding", &emb, &en);
    p.embedding = emb;
    p.embedding_dim = en;

    MemoryRecord *recs = NULL;
    size_t n = 0;
    aegis_status_t st = qe_search(db, &p, &recs, &n);
    free(tags);
    free(emb);
    if (st != AEGIS_OK) return json_error_status(st);

    cJSON *o = json_ok();
    cJSON *arr = cJSON_AddArrayToObject(o, "records");
    for (size_t i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, json_record(&recs[i]));
        record_free(&recs[i]);
    }
    free(recs);
    cJSON_AddNumberToObject(o, "total", (double)n);
    return o;
}

static cJSON *handle_promote(AegisDB *db, const cJSON *req) {
    const char *session = jr_str(req, "session_id", NULL);
    uint64_t wid;
    if (jr_u64(req, "working_id", &wid) != 0)
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    const char *to = jr_str(req, "to_type", "episodic");
    MemoryType tt;
    if (memory_type_from_string(to, &tt) != 0)
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    MemoryRecord out;
    aegis_status_t st = qe_promote(db, session, wid, tt, &out);
    if (st != AEGIS_OK) return json_error_status(st);
    cJSON *r = resp_record(&out);
    record_free(&out);
    return r;
}

static cJSON *handle_relate(AegisDB *db, const cJSON *req) {
    uint64_t from, to;
    if (jr_u64(req, "from_id", &from) != 0 || jr_u64(req, "to_id", &to) != 0)
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    const char *kind = jr_str(req, "kind", NULL);
    aegis_status_t st = qe_relate(db, from, to, kind);
    if (st != AEGIS_OK) return json_error_status(st);
    cJSON *o = json_ok();
    cJSON *rel = cJSON_AddObjectToObject(o, "relationship");
    cJSON_AddNumberToObject(rel, "from_id", (double)from);
    cJSON_AddNumberToObject(rel, "to_id", (double)to);
    if (kind) cJSON_AddStringToObject(rel, "kind", kind);
    return o;
}

static cJSON *handle_traverse(AegisDB *db, const cJSON *req) {
    uint64_t id;
    if (jr_u64(req, "id", &id) != 0)
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    uint64_t depth = 1;
    jr_u64(req, "depth", &depth);
    const char *agent = jr_str(req, "agent_id", NULL);
    MemoryRecord *recs = NULL;
    size_t n = 0;
    aegis_status_t st = qe_traverse(db, id, (int)depth, agent, &recs, &n);
    if (st != AEGIS_OK) return json_error_status(st);
    cJSON *o = json_ok();
    cJSON *arr = cJSON_AddArrayToObject(o, "records");
    for (size_t i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, json_record(&recs[i]));
        record_free(&recs[i]);
    }
    free(recs);
    cJSON_AddNumberToObject(o, "total", (double)n);
    return o;
}

/* Constant-time string equality: no early exit on mismatch, and length
 * differences fold into the result so timing does not leak the secret. */
static int ct_eq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la > lb ? la : lb;
    unsigned char diff = (unsigned char)(la ^ lb);
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned char)((i < la ? a[i] : 0) ^ (i < lb ? b[i] : 0));
    return diff == 0;
}

/* Authenticate a request against the configured token set. Auth is disabled
 * (always authorized) when no tokens are configured. Checks every token without
 * an early break so timing does not reveal which token matched. */
static int request_authorized(AegisDB *db, const cJSON *req) {
    if (db->config.auth_token_count == 0) return 1;
    const char *token = jr_str(req, "token", NULL);
    if (!token) return 0;
    int authorized = 0;
    for (size_t i = 0; i < db->config.auth_token_count; i++)
        if (ct_eq(token, db->config.auth_tokens[i])) authorized = 1;
    return authorized;
}

cJSON *query_engine_dispatch(AegisDB *db, const cJSON *req) {
    const char *op = jr_str(req, "operation", NULL);
    if (!op) return json_error_status(AEGIS_ERR_INVALID_REQUEST);

    /* "ping" is exempt so liveness and startup probes work unauthenticated. */
    if (strcmp(op, "ping") != 0 && !request_authorized(db, req))
        return json_error_status(AEGIS_ERR_UNAUTHORIZED);

    if (strcmp(op, "ping") == 0) return handle_ping(db);
    if (strcmp(op, "insert") == 0) return handle_insert(db, req);
    if (strcmp(op, "get") == 0) return handle_get(db, req);
    if (strcmp(op, "update") == 0) return handle_update(db, req);
    if (strcmp(op, "delete") == 0) return handle_delete(db, req);
    if (strcmp(op, "search") == 0) return handle_search(db, req);
    if (strcmp(op, "promote") == 0) return handle_promote(db, req);
    if (strcmp(op, "relate") == 0) return handle_relate(db, req);
    if (strcmp(op, "traverse") == 0) return handle_traverse(db, req);

    return json_error("INVALID_REQUEST", "unknown operation");
}