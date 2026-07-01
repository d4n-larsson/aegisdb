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
#include "aegisdb/logging.h"
#include "aegisdb/sha256.h"

#define MAX_TAGS 32     /* max tags per record (also enforced in validate_common) */
#define MAX_TOP_K 1000  /* clamp untrusted top_k to bound work/allocations */
#define SEARCH_FETCH_CAP 8192 /* upper bound on semantic over-fetch when widening
                               * to satisfy a selective filter (bounds worst-case
                               * work; a very selective filter may still yield
                               * fewer than top_k — inherent to filtered ANN) */
#define MAX_OFFSET 100000 /* clamp pagination offset to bound ranking work/allocs */

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
    if (r->tag_count > MAX_TAGS) return AEGIS_ERR_INVALID_REQUEST;
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

/* Read + decode a record for a read-only operation, keeping the disk I/O off
 * the index lock: resolve the id -> log offset under index_lock (read), pin the
 * log against compaction with log_lock (read), drop index_lock, then read. So
 * writers — which take index_lock for write but never log_lock — are not
 * blocked by the read's I/O. Caller must hold neither lock. */
static aegis_status_t load_record_ro(AegisDB *db, uint64_t id,
                                     MemoryRecord *out) {
    pthread_rwlock_rdlock(&db->index_lock);
    const HashEntry *e = hash_index_get(db->hash, id);
    if (!e) {
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_NOT_FOUND;
    }
    uint64_t off = e->offset;
    pthread_rwlock_rdlock(&db->log_lock);
    pthread_rwlock_unlock(&db->index_lock);
    uint8_t *buf = NULL;
    size_t len = 0;
    int rv = log_read(&db->log, off, &buf, &len);
    pthread_rwlock_unlock(&db->log_lock);
    if (rv != 0) return AEGIS_ERR_INTERNAL;
    rv = record_decode(buf, len, out);
    free(buf);
    return rv == 0 ? AEGIS_OK : AEGIS_ERR_INTERNAL;
}

/* Cross-tenant guard: when ns is set, a record whose agent_id the caller's
 * namespace does not own (different or absent) must be indistinguishable from
 * missing. agent_id is immutable per id, so checking it on the already-loaded
 * record is equivalent to the old separate ownership pre-read. */
static int ns_denies(const char *ns, const MemoryRecord *r) {
    return ns && (!r->agent_id || strcmp(r->agent_id, ns) != 0);
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
    if (st == AEGIS_OK) log_fsync_if_batched(&db->log); /* fsync off the lock */

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
    aegis_status_t st = load_record_ro(db, id, out);
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
                         const char *ns, MemoryRecord *out) {
    aegis_status_t st = require_phase(db, 2);
    if (st != AEGIS_OK) return st;

    pthread_rwlock_wrlock(&db->index_lock);
    MemoryRecord cur;
    st = load_record(db, id, &cur);
    if (st != AEGIS_OK) {
        pthread_rwlock_unlock(&db->index_lock);
        return st;
    }
    /* A record outside the caller's namespace reads back as missing, checked
     * before the type check so it cannot leak via an IMMUTABLE response. */
    if (cur.deleted || ns_denies(ns, &cur)) {
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
    if (st == AEGIS_OK) log_fsync_if_batched(&db->log); /* fsync off the lock */

    if (st != AEGIS_OK) {
        record_free(&cur);
        return st;
    }
    *out = cur; /* move ownership */
    return AEGIS_OK;
}

aegis_status_t qe_delete(AegisDB *db, uint64_t id, const char *ns) {
    aegis_status_t st = require_phase(db, 1);
    if (st != AEGIS_OK) return st;

    pthread_rwlock_wrlock(&db->index_lock);
    MemoryRecord cur;
    st = load_record(db, id, &cur);
    if (st != AEGIS_OK) {
        pthread_rwlock_unlock(&db->index_lock);
        return st;
    }
    if (cur.deleted || ns_denies(ns, &cur)) {
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
    if (st == AEGIS_OK) log_fsync_if_batched(&db->log); /* fsync off the lock */

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

/* A candidate's log offset + similarity score, snapshotted under the index lock
 * so the record body can be read off it. */
typedef struct {
    uint64_t off;
    float score;
} SearchSnap;

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

/* Bounded selection over candidate *indices*: a size-k heap whose root is the
 * worst-ranked kept candidate (per cmp; cmp(x,y)>0 means x ranks after y), so
 * the heap retains the k best. Operating on indices avoids moving the heavy
 * MemoryRecord values. */
static void idx_sift_down(size_t *h, size_t n, size_t i, const Cand *c,
                          int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, worst = i;
        if (l < n && cmp(&c[h[l]], &c[h[worst]]) > 0) worst = l;
        if (r < n && cmp(&c[h[r]], &c[h[worst]]) > 0) worst = r;
        if (worst == i) break;
        size_t t = h[i];
        h[i] = h[worst];
        h[worst] = t;
        i = worst;
    }
}

/* Resolve a candidate id set and load the surviving records. For semantic
 * queries the candidates are the top `fetch` by vector similarity; otherwise
 * the complete matching set from the time/tag index. Offsets are snapshotted
 * under index_lock, then records are read under log_lock (off the index lock)
 * and post-filtered. *exhausted is set when the candidate source returned fewer
 * than `fetch` (semantic) or is inherently complete (non-semantic) — i.e. there
 * is nothing more to find by widening. Allocates *out_cands (record_free each,
 * then free the array). 0/-1. */
static aegis_status_t gather_candidates(AegisDB *db, const SearchParams *p,
                                        size_t fetch, int semantic,
                                        Cand **out_cands, size_t *out_m,
                                        int *exhausted) {
    *out_cands = NULL;
    *out_m = 0;
    *exhausted = 1;
    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t nids = 0;

    pthread_rwlock_rdlock(&db->index_lock);
    if (semantic) {
        if (semantic_index_search(db->sem, p->embedding, p->embedding_dim, fetch,
                                  &ids, &scores, &nids) != 0) {
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
        *exhausted = (nids < fetch); /* fewer returned than asked -> saw them all */
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

    /* resolve id -> log offset under the index lock, snapshotting (offset,
     * similarity) so the disk reads can run off it */
    SearchSnap *snap = malloc((nids ? nids : 1) * sizeof(SearchSnap));
    if (!snap) {
        free(ids);
        free(scores);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_INTERNAL;
    }
    size_t sn = 0;
    for (size_t i = 0; i < nids; i++) {
        const HashEntry *e = hash_index_get(db->hash, ids[i]);
        if (!e) continue;
        snap[sn].off = e->offset;
        snap[sn].score = semantic ? scores[i] : 0.0f;
        sn++;
    }
    pthread_rwlock_rdlock(&db->log_lock); /* pin the log before dropping index */
    pthread_rwlock_unlock(&db->index_lock);
    free(ids);
    free(scores);

    /* load + decode + post-filter off the index lock; disk I/O holds only
     * log_lock, so concurrent writers are not blocked by it */
    Cand *cands = malloc((sn ? sn : 1) * sizeof(Cand));
    if (!cands) {
        pthread_rwlock_unlock(&db->log_lock);
        free(snap);
        return AEGIS_ERR_INTERNAL;
    }
    size_t m = 0;
    for (size_t i = 0; i < sn; i++) {
        /* min_score gates on the raw cosine similarity, before the log read */
        if (semantic && p->has_min_score && snap[i].score < p->min_score)
            continue;
        uint8_t *buf = NULL;
        size_t len = 0;
        if (log_read(&db->log, snap[i].off, &buf, &len) != 0) continue;
        MemoryRecord r;
        int dec = record_decode(buf, len, &r);
        free(buf);
        if (dec != 0) continue;
        if (!passes_filters(&r, p)) {
            record_free(&r);
            continue;
        }
        cands[m].rec = r;
        if (semantic) {
            /* T038: re-rank by importance * confidence * similarity */
            float sim = snap[i].score;
            float w = r.importance * r.confidence;
            cands[m].score = (w > 0 ? w : 1.0f) * sim;
        } else {
            cands[m].score = 0;
        }
        m++;
    }
    pthread_rwlock_unlock(&db->log_lock);
    free(snap);
    *out_cands = cands;
    *out_m = m;
    return AEGIS_OK;
}

aegis_status_t qe_search(AegisDB *db, const SearchParams *p,
                         MemoryRecord **out_records, size_t *out_n) {
    aegis_status_t st = require_phase(db, p->embedding_dim ? 3 : 2);
    if (st != AEGIS_OK) return st;
    if (p->embedding_dim &&
        p->embedding_dim != db->config.embedding_dimensions)
        return AEGIS_ERR_INVALID_REQUEST;

    size_t top_k = p->top_k ? p->top_k : 10;
    int semantic = p->embedding_dim ? 1 : 0;
    size_t offset = p->offset < MAX_OFFSET ? p->offset : MAX_OFFSET;
    /* rank enough to page past `offset` and still fill top_k */
    size_t want = offset + top_k;

    Cand *cands = NULL;
    size_t m = 0;
    if (semantic) {
        /* Over-fetch, then widen if a selective post-filter (or min_score, or a
         * page offset) leaves < want: the global vector index returns the
         * nearest regardless of filter, so a selective filter (e.g. a small
         * namespace) can drop them all. Re-query with a growing fetch until
         * enough survive, the index is exhausted, or the cap is hit. */
        size_t fetch = want * 4 + 32;
        if (fetch > SEARCH_FETCH_CAP) fetch = SEARCH_FETCH_CAP;
        for (;;) {
            int exhausted = 0;
            st = gather_candidates(db, p, fetch, 1, &cands, &m, &exhausted);
            if (st != AEGIS_OK) return st;
            if (m >= want || exhausted || fetch >= SEARCH_FETCH_CAP) break;
            for (size_t i = 0; i < m; i++) record_free(&cands[i].rec);
            free(cands);
            cands = NULL;
            m = 0;
            fetch = fetch * 4 < SEARCH_FETCH_CAP ? fetch * 4 : SEARCH_FETCH_CAP;
        }
    } else {
        int exhausted = 0;
        st = gather_candidates(db, p, 0, 0, &cands, &m, &exhausted);
        if (st != AEGIS_OK) return st;
    }

    /* Rank the best `sel_n` (= offset + top_k, capped at m) into `ranked`
     * (sorted best-first), then page: return the slice [offset, sel_n) and free
     * the paged-over head. */
    int (*cmp)(const void *, const void *) =
        semantic ? cmp_score_desc : cmp_created_asc;
    size_t sel_n = (want < m) ? want : m;
    Cand *ranked; /* length sel_n, sorted; owns .rec for [0, sel_n) */

    if (sel_n >= m) {
        /* ranking everything: a single full sort is simplest */
        qsort(cands, m, sizeof(Cand), cmp);
        ranked = cands; /* sel_n == m */
    } else {
        /* Select the sel_n best in O(m log sel_n) via a bounded index heap, then
         * sort those and free the records that did not make the cut. */
        size_t *heap = malloc(sel_n * sizeof(*heap));
        char *sel = calloc(m, 1);
        Cand *top = malloc(sel_n * sizeof(Cand));
        if (!heap || !sel || !top) {
            for (size_t i = 0; i < m; i++) record_free(&cands[i].rec);
            free(cands);
            free(heap);
            free(sel);
            free(top);
            return AEGIS_ERR_INTERNAL;
        }
        size_t hn = 0;
        for (size_t i = 0; i < m; i++) {
            if (hn < sel_n) {
                /* push: sift the new leaf up toward the (worst-ranked) root */
                size_t j = hn++;
                heap[j] = i;
                while (j > 0) {
                    size_t pa = (j - 1) / 2;
                    if (cmp(&cands[heap[j]], &cands[heap[pa]]) <= 0) break;
                    size_t t = heap[j];
                    heap[j] = heap[pa];
                    heap[pa] = t;
                    j = pa;
                }
            } else if (cmp(&cands[i], &cands[heap[0]]) < 0) {
                heap[0] = i; /* better than the worst kept: evict the root */
                idx_sift_down(heap, hn, 0, cands, cmp);
            }
        }
        for (size_t t = 0; t < sel_n; t++) {
            sel[heap[t]] = 1;
            top[t] = cands[heap[t]];
        }
        qsort(top, sel_n, sizeof(Cand), cmp);
        for (size_t i = 0; i < m; i++)
            if (!sel[i]) record_free(&cands[i].rec);
        free(heap);
        free(sel);
        free(cands);
        ranked = top;
    }

    /* page: keep [offset, sel_n), free the skipped head */
    size_t start = offset < sel_n ? offset : sel_n;
    size_t rn = sel_n - start;
    MemoryRecord *res = malloc((rn ? rn : 1) * sizeof(MemoryRecord));
    if (!res) {
        for (size_t i = 0; i < sel_n; i++) record_free(&ranked[i].rec);
        free(ranked);
        return AEGIS_ERR_INTERNAL;
    }
    for (size_t i = 0; i < start; i++) record_free(&ranked[i].rec);
    for (size_t i = start; i < sel_n; i++) res[i - start] = ranked[i].rec;
    free(ranked);
    *out_records = res;
    *out_n = rn;
    return AEGIS_OK;
}

aegis_status_t qe_promote(AegisDB *db, const char *session_id,
                          uint64_t working_id, MemoryType to_type,
                          const char *ns, MemoryRecord *out) {
    aegis_status_t st = require_phase(db, 4);
    if (st != AEGIS_OK) return st;
    if (!session_id) return AEGIS_ERR_INVALID_REQUEST;
    if (to_type != MEM_EPISODIC && to_type != MEM_SEMANTIC)
        return AEGIS_ERR_INVALID_REQUEST;

    MemoryRecord w;
    if (working_store_take(db->working, session_id, working_id, db_now_ms(), ns,
                           &w) != 0)
        return AEGIS_ERR_NOT_FOUND;

    /* re-insert as a persisted record, pinned to the caller's namespace */
    w.type = to_type;
    w.expires_at = 0;
    if (ns) {
        free(w.agent_id);
        w.agent_id = strdup(ns);
        if (!w.agent_id) {
            record_free(&w);
            return AEGIS_ERR_INTERNAL;
        }
    }
    st = qe_insert(db, &w, NULL, 0, out);
    record_free(&w);
    return st;
}

aegis_status_t qe_relate(AegisDB *db, uint64_t from_id, uint64_t to_id,
                         const char *kind, const char *ns) {
    aegis_status_t st = require_phase(db, 4);
    if (st != AEGIS_OK) return st;

    pthread_rwlock_wrlock(&db->index_lock);
    MemoryRecord from;
    st = load_record(db, from_id, &from);
    if (st != AEGIS_OK || from.deleted || ns_denies(ns, &from)) {
        if (st == AEGIS_OK) record_free(&from);
        pthread_rwlock_unlock(&db->index_lock);
        return st == AEGIS_OK ? AEGIS_ERR_NOT_FOUND : st;
    }
    /* The target must exist; a namespaced caller must additionally own it, so it
     * is loaded (not just existence-checked) to verify its namespace. Both cases
     * collapse to NOT_FOUND so neither leaks across tenants. */
    if (ns) {
        MemoryRecord to_rec;
        aegis_status_t tst = load_record(db, to_id, &to_rec);
        if (tst != AEGIS_OK || to_rec.deleted || ns_denies(ns, &to_rec)) {
            if (tst == AEGIS_OK) record_free(&to_rec);
            record_free(&from);
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_NOT_FOUND;
        }
        record_free(&to_rec);
    } else if (!hash_index_get(db->hash, to_id)) {
        record_free(&from);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_NOT_FOUND;
    }
    if (record_add_relationship(&from, from_id, to_id, kind) != 0) {
        record_free(&from);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_INTERNAL;
    }
    from.updated = db_now_ms();
    st = append_and_hash(db, &from); /* relationship metadata, content intact */
    pthread_rwlock_unlock(&db->index_lock);
    if (st == AEGIS_OK) log_fsync_if_batched(&db->log); /* fsync off the lock */
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

    for (int level = 0; level <= depth && front_n > 0; level++) {
        /* Resolve this level's not-yet-seen ids to log offsets under the index
         * lock, then read+decode them off it (disk I/O under log_lock only). */
        uint64_t *offs = malloc(front_n * sizeof(uint64_t));
        if (!offs) break; /* frontier freed after the loop; return what we have */
        size_t off_n = 0;

        pthread_rwlock_rdlock(&db->index_lock);
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
            const HashEntry *e = hash_index_get(db->hash, id);
            if (!e) continue;
            offs[off_n++] = e->offset;
        }
        pthread_rwlock_rdlock(&db->log_lock);
        pthread_rwlock_unlock(&db->index_lock);
        free(frontier);

        uint64_t *next = NULL;
        size_t next_n = 0, next_cap = 0;
        for (size_t i = 0; i < off_n; i++) {
            uint8_t *buf = NULL;
            size_t len = 0;
            if (log_read(&db->log, offs[i], &buf, &len) != 0) continue;
            MemoryRecord r;
            int dec = record_decode(buf, len, &r);
            free(buf);
            if (dec != 0) continue;
            if (r.deleted ||
                (agent_filter && (!r.agent_id ||
                                  strcmp(r.agent_id, agent_filter) != 0))) {
                /* a filtered node is skipped entirely, edges and all */
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
        pthread_rwlock_unlock(&db->log_lock);
        free(offs);
        frontier = next;
        front_n = next_n;
        (void)next_cap;
    }
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
    if (jr_str_array(req, "tags", &tags, &tn, MAX_TAGS) != 0) {
        *err = AEGIS_ERR_INVALID_REQUEST;
        return -1;
    }
    if (tn && record_set_tags(in, tags, tn) != 0) {
        free(tags);
        *err = AEGIS_ERR_INTERNAL;
        return -1;
    }
    free(tags);
    float *emb = NULL;
    size_t en = 0;
    if (jr_float_array(req, "embedding", &emb, &en,
                       db->config.embedding_dimensions) != 0) {
        *err = AEGIS_ERR_INVALID_REQUEST;
        return -1;
    }
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

/* Operational snapshot: durability posture, durability lag, record/index
 * counts. Read-only; intended for monitoring and capacity planning. */
static cJSON *handle_stats(AegisDB *db) {
    cJSON *o = json_ok();
    cJSON_AddStringToObject(o, "version", AEGIS_VERSION_STRING);
    cJSON_AddNumberToObject(o, "phase", db->config.enabled_phase);
    cJSON_AddNumberToObject(o, "uptime_ms",
                            (double)(db_now_ms() - db->started_ms));

    cJSON_AddStringToObject(o, "durability",
                            aegis_durability_name(db->config.durability));
    if (db->config.durability == AEGIS_DURABILITY_BATCH)
        cJSON_AddNumberToObject(o, "fsync_batch",
                                (double)db->config.fsync_batch_size);
    else if (db->config.durability == AEGIS_DURABILITY_INTERVAL)
        cJSON_AddNumberToObject(o, "fsync_interval_ms",
                                (double)db->config.fsync_interval_ms);

    pthread_rwlock_rdlock(&db->index_lock);
    size_t live = 0, tombstones = 0;
    const HashIndex *h = db->hash;
    for (size_t i = 0; i < h->cap; i++) {
        if (!h->buckets[i].used) continue;
        if (h->buckets[i].deleted) tombstones++;
        else live++;
    }
    cJSON_AddNumberToObject(o, "records", (double)live);
    cJSON_AddNumberToObject(o, "tombstones", (double)tombstones);
    cJSON_AddNumberToObject(o, "log_bytes", (double)db->log.size);
    cJSON_AddBoolToObject(o, "log_flush_pending", log_flush_pending(&db->log));

    cJSON *idx = cJSON_AddObjectToObject(o, "indexes");
    if (idx) {
        cJSON_AddNumberToObject(idx, "time", (double)db->time->n);
        cJSON_AddNumberToObject(idx, "tags", (double)tag_index_count(db->tags));
        cJSON_AddNumberToObject(idx, "semantic",
                                (double)semantic_index_count(db->sem));
        cJSON_AddNumberToObject(idx, "working",
                                (double)working_store_count(db->working));
    }
    pthread_rwlock_unlock(&db->index_lock);

    pthread_mutex_lock(&db->id_lock);
    cJSON_AddNumberToObject(o, "next_id", (double)db->next_id);
    pthread_mutex_unlock(&db->id_lock);
    return o;
}

static cJSON *handle_insert(AegisDB *db, const cJSON *req, const char *ns) {
    MemoryRecord in;
    aegis_status_t err;
    if (build_input_record(db, req, &in, &err) != 0) {
        record_free(&in);
        return json_error_status(err);
    }
    /* A namespaced token writes only into its own tenant: pin agent_id. */
    if (ns) {
        free(in.agent_id);
        in.agent_id = strdup(ns);
        if (!in.agent_id) {
            record_free(&in);
            return json_error_status(AEGIS_ERR_INTERNAL);
        }
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

static cJSON *handle_get(AegisDB *db, const cJSON *req, const char *ns) {
    uint64_t id;
    if (jr_u64(req, "id", &id) != 0) return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    const char *agent = ns ? ns : jr_str(req, "agent_id", NULL);
    MemoryRecord out;
    aegis_status_t st = qe_get(db, id, agent, &out);
    if (st != AEGIS_OK) return json_error_status(st);
    cJSON *r = resp_record(&out);
    record_free(&out);
    return r;
}

static cJSON *handle_update(AegisDB *db, const cJSON *req, const char *ns) {
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
        if (jr_str_array(req, "tags", &tags, &tn, MAX_TAGS) != 0)
            return json_error_status(AEGIS_ERR_INVALID_REQUEST);
        patch.has_tags = 1;
        patch.tags = tags;
        patch.tag_count = tn;
    }
    MemoryRecord out;
    aegis_status_t st = qe_update(db, id, &patch, ns, &out);
    free(tags);
    if (st != AEGIS_OK) return json_error_status(st);
    cJSON *r = resp_record(&out);
    record_free(&out);
    return r;
}

static cJSON *handle_delete(AegisDB *db, const cJSON *req, const char *ns) {
    uint64_t id;
    if (jr_u64(req, "id", &id) != 0)
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    aegis_status_t st = qe_delete(db, id, ns);
    if (st != AEGIS_OK) return json_error_status(st);
    cJSON *o = json_ok();
    cJSON_AddNumberToObject(o, "id", (double)id);
    cJSON_AddBoolToObject(o, "deleted", 1);
    return o;
}

static cJSON *handle_search(AegisDB *db, const cJSON *req, const char *ns) {
    SearchParams p;
    memset(&p, 0, sizeof(p));
    uint64_t v;
    if (jr_u64(req, "start_time", &p.start_time) == 0 &&
        jr_u64(req, "end_time", &p.end_time) == 0)
        p.has_time = 1;
    const char *type = jr_str(req, "type", NULL);
    if (type && memory_type_from_string(type, &p.type) == 0) p.has_type = 1;
    /* A namespaced token sees only its tenant; admin/no-auth may filter freely. */
    p.agent_id = ns ? ns : jr_str(req, "agent_id", NULL);
    const char *match = jr_str(req, "match", "all");
    p.match_all = (strcmp(match, "any") != 0);
    if (jr_u64(req, "top_k", &v) == 0)
        p.top_k = v > MAX_TOP_K ? MAX_TOP_K : (size_t)v; /* bound work/allocs */
    if (jr_u64(req, "offset", &v) == 0)
        p.offset = v > MAX_OFFSET ? MAX_OFFSET : (size_t)v; /* pagination */
    double ms;
    if (jr_f64(req, "min_score", &ms) == 0) {
        p.has_min_score = 1;
        p.min_score = (float)ms;
    }

    const char **tags = NULL;
    size_t tn = 0;
    if (jr_str_array(req, "tags", &tags, &tn, MAX_TAGS) != 0)
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    p.tags = tags;
    p.tag_count = tn;
    float *emb = NULL;
    size_t en = 0;
    if (jr_float_array(req, "embedding", &emb, &en,
                       db->config.embedding_dimensions) != 0) {
        free(tags);
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
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

static cJSON *handle_promote(AegisDB *db, const cJSON *req, const char *ns) {
    const char *session = jr_str(req, "session_id", NULL);
    uint64_t wid;
    if (jr_u64(req, "working_id", &wid) != 0)
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    const char *to = jr_str(req, "to_type", "episodic");
    MemoryType tt;
    if (memory_type_from_string(to, &tt) != 0)
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    MemoryRecord out;
    aegis_status_t st = qe_promote(db, session, wid, tt, ns, &out);
    if (st != AEGIS_OK) return json_error_status(st);
    cJSON *r = resp_record(&out);
    record_free(&out);
    return r;
}

static cJSON *handle_relate(AegisDB *db, const cJSON *req, const char *ns) {
    uint64_t from, to;
    if (jr_u64(req, "from_id", &from) != 0 || jr_u64(req, "to_id", &to) != 0)
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    const char *kind = jr_str(req, "kind", NULL);
    /* qe_relate enforces that both endpoints live in the caller's namespace */
    aegis_status_t st = qe_relate(db, from, to, kind, ns);
    if (st != AEGIS_OK) return json_error_status(st);
    cJSON *o = json_ok();
    cJSON *rel = cJSON_AddObjectToObject(o, "relationship");
    cJSON_AddNumberToObject(rel, "from_id", (double)from);
    cJSON_AddNumberToObject(rel, "to_id", (double)to);
    if (kind) cJSON_AddStringToObject(rel, "kind", kind);
    return o;
}

static cJSON *handle_traverse(AegisDB *db, const cJSON *req, const char *ns) {
    uint64_t id;
    if (jr_u64(req, "id", &id) != 0)
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    uint64_t depth = 1;
    jr_u64(req, "depth", &depth);
    const char *agent = ns ? ns : jr_str(req, "agent_id", NULL);
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

/* Constant-time fixed-length byte compare (no early exit). */
static int ct_eq_bytes(const uint8_t *a, const uint8_t *b, size_t n) {
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

/* Resolved caller identity for a request. */
typedef struct {
    const char *ns; /* bound namespace; NULL = unrestricted (admin / auth off) */
    int can_write;  /* 0 for read-only tokens */
} AuthCtx;

/* Authenticate a request and resolve the caller's namespace + scope. Auth is
 * disabled (unrestricted) when no tokens are configured. Every token is checked
 * without an early break so timing does not reveal which token matched. Returns
 * AEGIS_OK (ctx filled) or AEGIS_ERR_UNAUTHORIZED. */
static aegis_status_t resolve_identity(AegisDB *db, const cJSON *req,
                                       AuthCtx *out) {
    out->ns = NULL;
    out->can_write = 1;
    if (db->config.auth_token_count == 0) return AEGIS_OK; /* auth disabled */

    const char *token = jr_str(req, "token", NULL);
    const AuthToken *match = NULL;
    if (token) {
        uint8_t th[SHA256_DIGEST_LEN];
        int have_hash = 0;
        for (size_t i = 0; i < db->config.auth_token_count; i++) {
            const AuthToken *t = &db->config.auth_tokens[i];
            int eq;
            if (t->hashed) {
                if (!have_hash) {
                    sha256(token, strlen(token), th);
                    have_hash = 1;
                }
                eq = ct_eq_bytes(th, t->hash, SHA256_DIGEST_LEN);
            } else {
                eq = ct_eq(token, t->token);
            }
            if (eq) match = t;
        }
    }
    if (!match) return AEGIS_ERR_UNAUTHORIZED;
    out->ns = match->namespace; /* NULL for ADMIN tokens */
    out->can_write = (match->scope != AEGIS_SCOPE_RO);
    return AEGIS_OK;
}

static int is_write_op(const char *op) {
    return strcmp(op, "insert") == 0 || strcmp(op, "update") == 0 ||
           strcmp(op, "delete") == 0 || strcmp(op, "promote") == 0 ||
           strcmp(op, "relate") == 0;
}

cJSON *query_engine_dispatch(AegisDB *db, const cJSON *req) {
    const char *op = jr_str(req, "operation", NULL);
    if (!op) {
        LOG_DEBUG("dispatch: request with no \"operation\" field");
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }

    /* "ping" is exempt so liveness and startup probes work unauthenticated. */
    if (strcmp(op, "ping") == 0) return handle_ping(db);

    AuthCtx ctx;
    aegis_status_t ast = resolve_identity(db, req, &ctx);
    if (ast != AEGIS_OK) {
        LOG_WARN("dispatch: unauthorized \"%s\" request rejected", op);
        return json_error_status(ast);
    }
    if (is_write_op(op) && !ctx.can_write) {
        LOG_WARN("dispatch: read-only token attempted \"%s\"", op);
        return json_error_status(AEGIS_ERR_FORBIDDEN);
    }

    LOG_DEBUG("dispatch: operation \"%s\" (ns=%s)", op, ctx.ns ? ctx.ns : "*");

    if (strcmp(op, "stats") == 0) {
        /* stats exposes server-wide counts; restrict to admin/global tokens. */
        if (ctx.ns) return json_error_status(AEGIS_ERR_FORBIDDEN);
        return handle_stats(db);
    }
    if (strcmp(op, "insert") == 0) return handle_insert(db, req, ctx.ns);
    if (strcmp(op, "get") == 0) return handle_get(db, req, ctx.ns);
    if (strcmp(op, "update") == 0) return handle_update(db, req, ctx.ns);
    if (strcmp(op, "delete") == 0) return handle_delete(db, req, ctx.ns);
    if (strcmp(op, "search") == 0) return handle_search(db, req, ctx.ns);
    if (strcmp(op, "promote") == 0) return handle_promote(db, req, ctx.ns);
    if (strcmp(op, "relate") == 0) return handle_relate(db, req, ctx.ns);
    if (strcmp(op, "traverse") == 0) return handle_traverse(db, req, ctx.ns);

    LOG_WARN("dispatch: unknown operation \"%s\"", op);
    return json_error("INVALID_REQUEST", "unknown operation");
}