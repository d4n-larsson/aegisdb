/* Query engine — search, ranking, count (split from query_engine.c). */
#include "aegisdb/query_engine.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aegisdb/compaction.h"
#include "aegisdb/json_request.h"
#include "aegisdb/json_response.h"
#include "aegisdb/logging.h"
#include "aegisdb/replication.h"
#include "aegisdb/sha256.h"

#include "qe_internal.h"

/* predicate helpers for search */
static int rec_has_tag(const MemoryRecord *r, const char *tag) {
    for (size_t i = 0; i < r->tag_count; i++) {
        if (strcmp(r->tags[i], tag) == 0) {
            return 1;
        }
    }
    return 0;
}

static int passes_filters(const MemoryRecord *r, const SearchParams *p) {
    if (r->deleted) {
        return 0;
    }
    if (p->has_type && r->type != p->type) {
        return 0;
    }
    if (p->has_max_importance && r->importance > p->max_importance) {
        return 0;
    }
    if (p->agent_id &&
        (!r->agent_id || strcmp(r->agent_id, p->agent_id) != 0)) {
        return 0;
    }
    if (p->has_time &&
        (r->created < p->start_time || r->created > p->end_time)) {
        return 0;
    }
    if (p->tag_count) {
        if (p->match_all) {
            for (size_t i = 0; i < p->tag_count; i++) {
                if (!rec_has_tag(r, p->tags[i])) {
                    return 0;
                }
            }
        } else {
            int any = 0;
            for (size_t i = 0; i < p->tag_count; i++) {
                if (rec_has_tag(r, p->tags[i])) {
                    any = 1;
                    break;
                }
            }
            if (!any) {
                return 0;
            }
        }
    }
    return 1;
}

/* A candidate's log offset + similarity score, snapshotted under the index lock
 * so the record body can be read off it. */
typedef struct {
    uint64_t off;
    float score;
} SearchSnap;

static int cmp_score_desc(const void *a, const void *b) {
    float x = ((const Cand *)a)->score;
    float y = ((const Cand *)b)->score;
    if (x < y) {
        return 1;
    }
    if (x > y) {
        return -1;
    }
    return 0;
}
static int cmp_created_asc(const void *a, const void *b) {
    uint64_t x = ((const Cand *)a)->rec.created;
    uint64_t y = ((const Cand *)b)->rec.created;
    if (x < y) {
        return -1;
    }
    if (x > y) {
        return 1;
    }
    return 0;
}

/* Bounded selection over candidate *indices*: a size-k heap whose root is the
 * worst-ranked kept candidate (per cmp; cmp(x,y)>0 means x ranks after y), so
 * the heap retains the k best. Operating on indices avoids moving the heavy
 * MemoryRecord values. */
static void idx_sift_down(size_t *h, size_t n, size_t i, const Cand *c,
                          int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t l = (2 * i) + 1;
        size_t r = (2 * i) + 2;
        size_t worst = i;
        if (l < n && cmp(&c[h[l]], &c[h[worst]]) > 0) {
            worst = l;
        }
        if (r < n && cmp(&c[h[r]], &c[h[worst]]) > 0) {
            worst = r;
        }
        if (worst == i) {
            break;
        }
        size_t t = h[i];
        h[i] = h[worst];
        h[worst] = t;
        i = worst;
    }
}

/* Sift the leaf at index `i` up toward the root of the (max-by-cmp) heap — the
 * insert-side mirror of idx_sift_down. */
static void idx_sift_up(size_t *h, size_t i, const Cand *c,
                        int (*cmp)(const void *, const void *)) {
    while (i > 0) {
        size_t pa = (i - 1) / 2;
        if (cmp(&c[h[i]], &c[h[pa]]) <= 0) {
            break;
        }
        size_t t = h[i];
        h[i] = h[pa];
        h[pa] = t;
        i = pa;
    }
}

/* Select and sort the best `sel_n` (<= m) candidates by `cmp`, best-first.
 * Consumes `cands`: returns a freshly-owned array of length sel_n (whose records
 * it owns) and frees the records of the m-sel_n that did not make the cut, or
 * returns NULL on OOM (all m records + cands freed). When sel_n == m the array
 * is `cands` sorted in place. */
static Cand *select_top(Cand *cands, size_t m, size_t sel_n,
                        int (*cmp)(const void *, const void *)) {
    if (sel_n >= m) {
        /* ranking everything: a single full sort is simplest */
        qsort(cands, m, sizeof(Cand), cmp);
        return cands;
    }
    /* Select the sel_n best in O(m log sel_n) via a bounded index heap, then sort
     * those and free the records that did not make the cut. */
    size_t *heap = malloc(sel_n * sizeof(*heap));
    char *sel = calloc(m, 1);
    Cand *top = malloc(sel_n * sizeof(Cand));
    if (!heap || !sel || !top) {
        for (size_t i = 0; i < m; i++) {
            record_free(&cands[i].rec);
        }
        free(cands);
        free(heap);
        free(sel);
        free(top);
        return NULL;
    }
    size_t hn = 0;
    for (size_t i = 0; i < m; i++) {
        if (hn < sel_n) {
            heap[hn] = i;
            idx_sift_up(heap, hn, cands, cmp);
            hn++;
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
    for (size_t i = 0; i < m; i++) {
        if (!sel[i]) {
            record_free(&cands[i].rec);
        }
    }
    free(heap);
    free(sel);
    free(cands);
    return top;
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
                                        size_t load_cap, Cand **out_cands,
                                        size_t *out_m, int *exhausted) {
    *out_cands = NULL;
    *out_m = 0;
    *exhausted = 1;
    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t nids = 0;

    pthread_rwlock_rdlock(&db->index_lock);
    if (semantic) {
        if (semantic_index_search(db->sem, p->embedding, p->embedding_dim,
                                  fetch, &ids, &scores, &nids) != 0) {
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
        *exhausted =
            (nids < fetch); /* fewer returned than asked -> saw them all */
    } else if (p->has_time) {
        /* A wide-open time range is effectively a full scan, so bound it to
         * `load_cap` (0 = unlimited); *exhausted stays set only if nothing was
         * dropped. Default keeps the most-recent cap-worth so fresh writes are
         * never hidden; `oldest_first` keeps the OLDEST instead (candidate
         * selection wants the aging tail, not the recent one). */
        int trunc = 0;
        int rc =
            p->oldest_first
                ? time_index_range(db->time, p->start_time, p->end_time,
                                   load_cap, &ids, &nids)
                : time_index_range_recent(db->time, p->start_time, p->end_time,
                                          load_cap, &ids, &nids, &trunc);
        if (rc != 0) {
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
        /* time_index_range caps to the oldest `load_cap` and returns nids==cap
         * when it truncates; treat a full load as "possibly more". */
        *exhausted =
            p->oldest_first ? (load_cap == 0 || nids < load_cap) : !trunc;
    } else if (p->tag_count) {
        if (tag_index_query(db->tags, p->tags, p->tag_count, p->match_all, &ids,
                            &nids) != 0) {
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
    } else {
        /* No positive filter: scan live records, but load at most the most-recent
         * `load_cap` so an unfiltered query cannot pull the whole dataset into
         * RAM (amplification DoS). */
        int trunc = 0;
        if (time_index_range_recent(db->time, 0, UINT64_MAX, load_cap, &ids,
                                    &nids, &trunc) != 0) {
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
        *exhausted = !trunc;
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
        if (!e) {
            continue;
        }
        snap[sn].off = e->offset;
        snap[sn].score = semantic ? scores[i] : 0.0F;
        sn++;
    }
    pthread_rwlock_rdlock(
        &db->log_lock); /* pin the log before dropping index */
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
    /* clock sampled once so expiry + recency decay rank all candidates
     * consistently within this query */
    uint64_t now = db_now_ms();
    size_t m = 0;
    for (size_t i = 0; i < sn; i++) {
        /* min_score gates on the raw cosine similarity, before the log read */
        if (semantic && p->has_min_score && snap[i].score < p->min_score) {
            continue;
        }
        uint8_t *buf = NULL;
        size_t len = 0;
        if (log_read(&db->log, snap[i].off, &buf, &len) != 0) {
            continue;
        }
        MemoryRecord r;
        int dec = record_decode(buf, len, &r);
        free(buf);
        if (dec != 0) {
            continue;
        }
        if (record_expired(&r, now) || !passes_filters(&r, p)) {
            record_free(&r);
            continue;
        }
        cands[m].rec = r;
        if (semantic) {
            /* T038: re-rank by importance * confidence * similarity, then (#69)
             * an optional exponential recency decay by age since `updated`. */
            float sim = snap[i].score;
            float w = r.importance * r.confidence;
            float wapplied = w > 0 ? w : 1.0F;
            float recency = 1.0F;
            if (p->half_life_ms) {
                double age = now > r.updated ? (double)(now - r.updated) : 0.0;
                /* 0.5^(age/half_life) == exp(-ln2 * age/half_life) */
                recency = (float)exp(-0.6931471805599453 * age /
                                     (double)p->half_life_ms);
            }
            cands[m].sim = sim;
            cands[m].weight = wapplied;
            cands[m].recency = recency;
            cands[m].score = wapplied * sim * recency;
        } else {
            cands[m].sim = 0;
            cands[m].weight = r.importance * r.confidence;
            cands[m].recency = 1.0F;
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
    return qe_search_ex(db, p, out_records, NULL, out_n);
}

aegis_status_t qe_search_ex(AegisDB *db, const SearchParams *p,
                            MemoryRecord **out_records,
                            SearchExplain **out_explain, size_t *out_n) {
    aegis_status_t st = require_phase(db, p->embedding_dim ? 3 : 2);
    if (st != AEGIS_OK) {
        return st;
    }
    if (p->embedding_dim &&
        p->embedding_dim != db->config.embedding_dimensions) {
        return AEGIS_ERR_INVALID_REQUEST;
    }

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
        size_t fetch = (want * 4) + 32;
        if (fetch > SEARCH_FETCH_CAP) {
            fetch = SEARCH_FETCH_CAP;
        }
        for (;;) {
            int exhausted = 0;
            st = gather_candidates(db, p, fetch, 1, 0, &cands, &m, &exhausted);
            if (st != AEGIS_OK) {
                return st;
            }
            if (m >= want || exhausted || fetch >= SEARCH_FETCH_CAP) {
                break;
            }
            for (size_t i = 0; i < m; i++) {
                record_free(&cands[i].rec);
            }
            free(cands);
            cands = NULL;
            m = 0;
            fetch = fetch * 4 < SEARCH_FETCH_CAP ? fetch * 4 : SEARCH_FETCH_CAP;
        }
    } else {
        int exhausted = 0;
        st = gather_candidates(db, p, 0, 0, db->config.query_scan_cap, &cands,
                               &m, &exhausted);
        if (st != AEGIS_OK) {
            return st;
        }
    }

    /* Rank the best `sel_n` (= offset + top_k, capped at m) into `ranked`
     * (sorted best-first), then page: return the slice [offset, sel_n) and free
     * the paged-over head. */
    int (*cmp)(const void *, const void *) =
        semantic ? cmp_score_desc : cmp_created_asc;
    size_t sel_n = (want < m) ? want : m;
    /* Rank the best `sel_n` into `ranked` (best-first); it owns .rec for
     * [0, sel_n). NULL is OOM only when sel_n > 0 (sel_n == 0 == no candidates). */
    Cand *ranked = select_top(cands, m, sel_n, cmp);
    if (sel_n > 0 && !ranked) {
        return AEGIS_ERR_INTERNAL;
    }

    /* page: keep [offset, sel_n), free the skipped head */
    size_t start = offset < sel_n ? offset : sel_n;
    size_t rn = sel_n - start;
    MemoryRecord *res = malloc((rn ? rn : 1) * sizeof(MemoryRecord));
    SearchExplain *exp = NULL;
    if (out_explain) {
        exp = malloc((rn ? rn : 1) * sizeof(SearchExplain));
        if (!exp) {
            free(res);
            for (size_t i = 0; i < sel_n; i++) {
                record_free(&ranked[i].rec);
            }
            free(ranked);
            return AEGIS_ERR_INTERNAL;
        }
    }
    if (!res) {
        free(exp);
        for (size_t i = 0; i < sel_n; i++) {
            record_free(&ranked[i].rec);
        }
        free(ranked);
        return AEGIS_ERR_INTERNAL;
    }
    for (size_t i = 0; i < start; i++) {
        record_free(&ranked[i].rec);
    }
    for (size_t i = start; i < sel_n; i++) {
        res[i - start] = ranked[i].rec;
        if (exp) {
            exp[i - start] = (SearchExplain){
                .semantic = semantic,
                .similarity = ranked[i].sim,
                .importance = ranked[i].rec.importance,
                .confidence = ranked[i].rec.confidence,
                .weight = ranked[i].weight,
                .recency_factor = ranked[i].recency,
                .score = ranked[i].score,
            };
        }
    }
    free(ranked);
    *out_records = res;
    if (out_explain) {
        *out_explain = exp;
    }
    *out_n = rn;
    return AEGIS_OK;
}

/* Count live records matching the filters (type/tags/time/agent_id). Ignores
 * any embedding — count is over the filter predicate, not vector ranking. */
aegis_status_t qe_count(AegisDB *db, const SearchParams *p, size_t *out_count,
                        int *out_capped) {
    aegis_status_t st = require_phase(db, 2);
    if (st != AEGIS_OK) {
        return st;
    }
    Cand *cands = NULL;
    size_t m = 0;
    int exhausted = 0;
    st = gather_candidates(db, p, 0, 0, db->config.query_scan_cap, &cands, &m,
                           &exhausted);
    if (st != AEGIS_OK) {
        return st;
    }
    for (size_t i = 0; i < m; i++) {
        record_free(&cands[i].rec);
    }
    free(cands);
    *out_count = m;
    /* When the broad-scan cap truncated the candidate set the count is a floor,
     * not exact — tell the caller so it isn't reported as authoritative. */
    if (out_capped) {
        *out_capped = !exhausted;
    }
    return AEGIS_OK;
}

/* Delete every live record matching the filters, scoped to `ns` when set.
 * Requires at least one positive filter (type/tags/time) so an unfiltered
 * "delete everything" is not possible by omission. Returns the count deleted. */
aegis_status_t qe_delete_by_query(AegisDB *db, const SearchParams *p,
                                  const char *ns, size_t *out_deleted) {
    aegis_status_t st = require_phase(db, 1);
    if (st != AEGIS_OK) {
        return st;
    }
    if (!p->has_type && !p->tag_count && !p->has_time) {
        return AEGIS_ERR_INVALID_REQUEST; /* refuse an unfiltered bulk delete */
    }

    /* Delete must act on the complete matching set (a partial delete would
     * silently leave data behind), so it is not scan-capped; the mandatory
     * filter above already bounds an unfiltered "delete everything". */
    Cand *cands = NULL;
    size_t m = 0;
    int exhausted = 0;
    st = gather_candidates(db, p, 0, 0, 0, &cands, &m, &exhausted);
    if (st != AEGIS_OK) {
        return st;
    }

    /* snapshot the matching ids, then release the loaded records — qe_delete
     * re-loads and re-validates each under the write lock (namespace included),
     * so a racing change just yields NOT_FOUND for that id and is skipped. */
    uint64_t *ids = malloc((m ? m : 1) * sizeof(uint64_t));
    if (!ids) {
        for (size_t i = 0; i < m; i++) {
            record_free(&cands[i].rec);
        }
        free(cands);
        return AEGIS_ERR_INTERNAL;
    }
    for (size_t i = 0; i < m; i++) {
        ids[i] = cands[i].rec.id;
        record_free(&cands[i].rec);
    }
    free(cands);

    size_t deleted = 0;
    for (size_t i = 0; i < m; i++) {
        if (qe_delete(db, ids[i], ns) == AEGIS_OK) {
            deleted++;
        }
    }
    free(ids);
    *out_deleted = deleted;
    return AEGIS_OK;
}
