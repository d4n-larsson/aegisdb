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

/* Does this record's fact match the bound positions of the pattern? Exact, and
 * index-free: the record is already loaded, so this compares what it actually
 * asserts rather than trusting the candidate source. That matters because the
 * pattern is also usable *alongside* a semantic or lexical query, where the
 * candidates come from somewhere else entirely. */
static int matches_pattern(const MemoryRecord *r, const SearchParams *p) {
    if (r->fact.kind == FACT_NONE || !r->fact.predicate) {
        return 0; /* a record asserting nothing matches no pattern */
    }
    if (p->pat_has_subject && r->fact.subject != p->pat_subject) {
        /* With `subsume`, a fact about anything that reaches the bound subject
         * through `is_a` answers too — a memory about hnsw.c answering a
         * question about the storage layer. Membership is checked here, on the
         * loaded record, for the same reason every other position is: the
         * candidate source is not always the fact index. */
        int member = 0;
        for (size_t i = 0; i < p->subsume_n; i++) {
            if (p->subsume_ids[i] == r->fact.subject) {
                member = 1;
                break;
            }
        }
        if (!member) {
            return 0;
        }
    }
    if (p->pat_predicate && strcmp(r->fact.predicate, p->pat_predicate) != 0) {
        return 0;
    }
    if (p->pat_has_object) {
        if (r->fact.kind != p->pat_object_kind) {
            return 0; /* an id never matches a literal, or the reverse */
        }
        if (p->pat_object_kind == FACT_OBJ_ID) {
            if (r->fact.object_id != p->pat_object_id) {
                return 0;
            }
        } else if (!r->fact.object_str || !p->pat_object_str ||
                   strcmp(r->fact.object_str, p->pat_object_str) != 0) {
            return 0;
        }
    }
    return 1;
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
    if (p->has_pattern && !matches_pattern(r, p)) {
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

/* Where a query's candidate ids come from, and whether they arrive ranked. */
typedef enum {
    CAND_FILTER = 0, /* time / tags / scan — time-ordered, unscored */
    CAND_SEMANTIC,   /* vector ANN, ranked by cosine similarity */
    CAND_LEXICAL,    /* BM25 over the payload text (ROADMAP 4.1) */
    CAND_HYBRID      /* both, fused by reciprocal rank */
} CandSource;

/* One candidate id with the full ranking breakdown assembled for it, so a hybrid
 * hit can explain which retrieval path found it (ROADMAP 1.2). */
typedef struct {
    uint64_t id;
    float score; /* the raw relevance this candidate ranks on */
    float sim;
    float bm25;
    float rrf;
    uint32_t srank; /* 1-based rank in the semantic list; 0 = absent */
    uint32_t lrank; /* 1-based rank in the lexical list; 0 = absent */
} ScoredId;

/* A candidate's log offset + its ranking breakdown, snapshotted under the index
 * lock so the record body can be read off it. */
typedef struct {
    uint64_t off;
    ScoredId sc;
} SearchSnap;

static int cmp_scored_by_id(const void *a, const void *b) {
    uint64_t x = ((const ScoredId *)a)->id;
    uint64_t y = ((const ScoredId *)b)->id;
    if (x < y) {
        return -1;
    }
    if (x > y) {
        return 1;
    }
    return 0;
}

/* Fuse a semantic and a lexical result list by reciprocal rank:
 *   score = sum over the lists containing the record of 1/(RRF_K + rank).
 *
 * Rank-based rather than score-based deliberately: a cosine in [-1,1] and an
 * unbounded BM25 score share no scale, and normalising them per query would make
 * each record's score depend on the rest of the batch (so adding one document
 * could reorder unrelated results). Ranks are comparable by construction.
 *
 * Consumes nothing; allocates the fused array (free with free()). Returns NULL
 * on allocation failure. */
static ScoredId *rrf_fuse(const uint64_t *sids, const float *sscores, size_t sn,
                          const uint64_t *lids, const float *lscores, size_t ln,
                          size_t *out_n) {
    *out_n = 0;
    size_t total = sn + ln;
    ScoredId *all = malloc((total ? total : 1) * sizeof(*all));
    if (!all) {
        return NULL;
    }
    size_t n = 0;
    for (size_t i = 0; i < sn; i++) {
        all[n] = (ScoredId){
            .id = sids[i], .sim = sscores[i], .srank = (uint32_t)(i + 1)};
        n++;
    }
    for (size_t i = 0; i < ln; i++) {
        all[n] = (ScoredId){
            .id = lids[i], .bm25 = lscores[i], .lrank = (uint32_t)(i + 1)};
        n++;
    }
    /* Merge the two lists on id: sort, then fold each run of equal ids into one
     * entry carrying both ranks. */
    qsort(all, n, sizeof(*all), cmp_scored_by_id);
    size_t m = 0;
    for (size_t i = 0; i < n;) {
        ScoredId cur = all[i];
        size_t j = i + 1;
        for (; j < n && all[j].id == cur.id; j++) {
            if (all[j].srank) {
                cur.srank = all[j].srank;
                cur.sim = all[j].sim;
            }
            if (all[j].lrank) {
                cur.lrank = all[j].lrank;
                cur.bm25 = all[j].bm25;
            }
        }
        float rrf = 0.0F;
        if (cur.srank) {
            rrf += 1.0F / (float)(RRF_K + cur.srank);
        }
        if (cur.lrank) {
            rrf += 1.0F / (float)(RRF_K + cur.lrank);
        }
        cur.rrf = rrf;
        cur.score = rrf;
        all[m++] = cur;
        i = j;
    }
    *out_n = m;
    return all;
}

static int cmp_score_desc(const void *a, const void *b) {
    float x = ((const Cand *)a)->score;
    float y = ((const Cand *)b)->score;
    if (x < y) {
        return 1;
    }
    if (x > y) {
        return -1;
    }
    /* Equal scores break on ascending id, so the ranking is deterministic and
     * paging is stable. qsort is not a stable sort, and exact ties are routine
     * under fusion: a record at rank 1 of the lexical list scores exactly what a
     * record at rank 1 of the semantic list does. */
    uint64_t i = ((const Cand *)a)->rec.id;
    uint64_t j = ((const Cand *)b)->rec.id;
    if (i < j) {
        return -1;
    }
    if (i > j) {
        return 1;
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

/* Resolve a candidate id set and load the surviving records. A ranked source
 * (semantic / lexical / hybrid) contributes its top `fetch`; CAND_FILTER
 * contributes the complete matching set from the time/tag index. Offsets are
 * snapshotted under index_lock, then records are read under log_lock (off the
 * index lock) and post-filtered. *exhausted is set when the candidate source
 * returned fewer than `fetch`, or is inherently complete — i.e. there is nothing
 * more to find by widening. Allocates *out_cands (record_free each, then free
 * the array). 0/-1. */
static aegis_status_t gather_candidates(AegisDB *db, const SearchParams *p,
                                        size_t fetch, CandSource src,
                                        size_t load_cap, Cand **out_cands,
                                        size_t *out_m, int *exhausted) {
    *out_cands = NULL;
    *out_m = 0;
    *exhausted = 1;
    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t nids = 0;
    ScoredId *sc = NULL; /* ranked sources only */
    int ranked = (src != CAND_FILTER);

    pthread_rwlock_rdlock(&db->index_lock);
    if (src == CAND_SEMANTIC || src == CAND_HYBRID) {
        if (semantic_index_search(db->sem, p->embedding, p->embedding_dim,
                                  fetch, &ids, &scores, &nids) != 0) {
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
        /* Fewer returned than asked -> the index has nothing more to give.
         * Measured before min_score trims the list, so widening is driven by
         * what the index holds, not by how much the caller filtered out. */
        *exhausted = (nids < fetch);
        /* min_score gates on the raw cosine, before the fusion and the log
         * read, so it means the same thing in a hybrid query as it does alone. */
        if (p->has_min_score) {
            size_t keep = 0;
            for (size_t i = 0; i < nids; i++) {
                if (scores[i] >= p->min_score) {
                    ids[keep] = ids[i];
                    scores[keep] = scores[i];
                    keep++;
                }
            }
            nids = keep;
        }
    }
    if (src == CAND_LEXICAL || src == CAND_HYBRID) {
        uint64_t *lids = NULL;
        float *lscores = NULL;
        size_t lnids = 0;
        if (lexical_index_search(db->lex, p->query, fetch, &lids, &lscores,
                                 &lnids) != 0) {
            free(ids);
            free(scores);
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
        if (src == CAND_LEXICAL) {
            *exhausted = (lnids < fetch);
            sc = malloc((lnids ? lnids : 1) * sizeof(*sc));
            if (sc) {
                for (size_t i = 0; i < lnids; i++) {
                    sc[i] = (ScoredId){.id = lids[i],
                                       .score = lscores[i],
                                       .bm25 = lscores[i],
                                       .lrank = (uint32_t)(i + 1)};
                }
                nids = lnids;
            }
        } else {
            /* Widening only helps while *both* sources still have more. */
            *exhausted = *exhausted && (lnids < fetch);
            size_t fn = 0;
            sc = rrf_fuse(ids, scores, nids, lids, lscores, lnids, &fn);
            nids = fn;
        }
        free(lids);
        free(lscores);
        free(ids);
        free(scores);
        ids = NULL;
        scores = NULL;
        if (!sc) {
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
    } else if (src == CAND_SEMANTIC) {
        sc = malloc((nids ? nids : 1) * sizeof(*sc));
        if (!sc) {
            free(ids);
            free(scores);
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
        for (size_t i = 0; i < nids; i++) {
            sc[i] = (ScoredId){.id = ids[i],
                               .score = scores[i],
                               .sim = scores[i],
                               .srank = (uint32_t)(i + 1)};
        }
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
            /* Every other branch here leaves *ids NULL on failure, so this
             * used to need no free. The subsume union accumulates across
             * lookups, so it does. */
            free(ids);
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
        /* time_index_range caps to the oldest `load_cap` and returns nids==cap
         * when it truncates; treat a full load as "possibly more". */
        *exhausted =
            p->oldest_first ? (load_cap == 0 || nids < load_cap) : !trunc;
    } else if (p->has_pattern) {
        /* Draw candidates from whichever bound position the index can answer
         * most narrowly: a subject or an object each pins one slot, while a
         * predicate alone pins a whole list. The predicate is passed along in
         * the first two cases so it narrows inside the index rather than in the
         * post-filter. The set is complete either way, so nothing is exhausted
         * early. */
        int rc;
        if (p->pat_has_subject && p->subsume_n) {
            /* One lookup per subject in the expansion, unioned. The exact
             * post-filter above still decides, so a duplicate id here costs a
             * reload rather than a wrong answer. */
            rc = 0;
            for (size_t i = 0; i <= p->subsume_n && rc == 0; i++) {
                uint64_t subj =
                    (i == 0) ? p->pat_subject : p->subsume_ids[i - 1];
                uint64_t *part = NULL;
                size_t pn = 0;
                rc = fact_index_by_subject(db->facts, subj, p->pat_predicate,
                                           &part, &pn);
                if (rc != 0 || pn == 0) {
                    free(part);
                    continue;
                }
                uint64_t *grown = realloc(ids, (nids + pn) * sizeof(*grown));
                if (!grown) {
                    free(part);
                    rc = -1;
                    break; /* `ids` is freed by the rc != 0 path below */
                }
                ids = grown;
                memcpy(ids + nids, part, pn * sizeof(*part));
                nids += pn;
                free(part);
            }
        } else if (p->pat_has_subject) {
            rc = fact_index_by_subject(db->facts, p->pat_subject,
                                       p->pat_predicate, &ids, &nids);
        } else if (p->pat_has_object) {
            rc = fact_index_by_object(db->facts, p->pat_object_kind,
                                      p->pat_object_id, p->pat_object_str,
                                      p->pat_predicate, &ids, &nids);
        } else {
            rc = fact_index_by_predicate(db->facts, p->pat_predicate, &ids,
                                         &nids);
        }
        if (rc != 0) {
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
     * ranking breakdown) so the disk reads can run off it */
    SearchSnap *snap = malloc((nids ? nids : 1) * sizeof(SearchSnap));
    if (!snap) {
        free(ids);
        free(scores);
        free(sc);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_INTERNAL;
    }
    size_t sn = 0;
    for (size_t i = 0; i < nids; i++) {
        uint64_t id = ranked ? sc[i].id : ids[i];
        const HashEntry *e = hash_index_get(db->hash, id);
        if (!e) {
            continue;
        }
        snap[sn].off = e->offset;
        snap[sn].sc = ranked ? sc[i] : (ScoredId){.id = id};
        sn++;
    }
    pthread_rwlock_rdlock(
        &db->log_lock); /* pin the log before dropping index */
    pthread_rwlock_unlock(&db->index_lock);
    free(ids);
    free(scores);
    free(sc);

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
        if (ranked) {
            /* T038: re-rank the source's relevance by importance * confidence,
             * then (#69) an optional exponential recency decay by age since
             * `updated`. The relevance is the cosine (semantic) or the BM25 score
             * (lexical).
             *
             * A fused (hybrid) query is deliberately NOT weighted this way.
             * Reciprocal-rank scores are near-uniform by construction — rank 1
             * and rank 2 differ by 1/61 vs 1/62, under 2% — so any multiplier
             * with a wider spread than that becomes the primary sort key instead
             * of a modifier. Multiplying by importance*confidence (commonly
             * 0.5..1.0) demoted correct top-1 hits below merely-important ones
             * and measurably cost recall@1 in `make eval EVAL_ARGS='--lexical'`.
             * So a hybrid query ranks on the fusion alone, and reports weight and
             * recency_factor as 1.0 — "not applied" — which keeps the documented
             * score == weight * relevance * recency_factor identity true. Use a
             * single-source query when importance or half_life_ms shaping is what
             * you want. */
            int fused = (src == CAND_HYBRID);
            float relevance = snap[i].sc.score;
            float w = r.importance * r.confidence;
            float wapplied = (fused || w <= 0) ? 1.0F : w;
            float recency = 1.0F;
            if (p->half_life_ms && !fused) {
                double age = now > r.updated ? (double)(now - r.updated) : 0.0;
                /* 0.5^(age/half_life) == exp(-ln2 * age/half_life) */
                recency = (float)exp(-0.6931471805599453 * age /
                                     (double)p->half_life_ms);
            }
            cands[m].sim = snap[i].sc.sim;
            cands[m].bm25 = snap[i].sc.bm25;
            cands[m].rrf = snap[i].sc.rrf;
            cands[m].srank = snap[i].sc.srank;
            cands[m].lrank = snap[i].sc.lrank;
            cands[m].weight = wapplied;
            cands[m].recency = recency;
            cands[m].score = wapplied * relevance * recency;
        } else {
            cands[m].sim = 0;
            cands[m].bm25 = 0;
            cands[m].rrf = 0;
            cands[m].srank = 0;
            cands[m].lrank = 0;
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
    /* A copy, so this one keeps its const contract: qe_search_ex writes
     * subsume_truncated back, and a caller of the plain form has nowhere to
     * read it from anyway. */
    SearchParams q = *p;
    return qe_search_ex(db, &q, out_records, NULL, out_n);
}

/* Resolve `subsume`: every record that reaches the bound subject through the
 * `is_a` taxonomy.
 *
 * One index lookup, not a walk — the inference job materializes `is_a`'s
 * transitive closure, so a single {p: "is_a", o: {id: S}} query already names
 * every descendant however deep. That composition is why subsumption is a
 * query-time expansion here rather than the third closure the roadmap asked
 * for: materializing it would write facts that are false (the storage layer
 * does not default to what hnsw.c defaults to) and go quadratic in taxonomy
 * depth times facts per entity.
 *
 * The is_a records name the descendant in their *subject*, which the index
 * does not carry, so each one is loaded. Bounded by the cap for exactly that
 * reason. Returns 0, or -1 on allocation failure; a missing index or an empty
 * taxonomy is simply an empty expansion. */
static int cmp_u64_asc(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int resolve_subsume(AegisDB *db, SearchParams *p) {
    p->subsume_ids = NULL;
    p->subsume_n = 0;
    p->subsume_truncated = 0;
    if (!p->subsume || !p->pat_has_subject || !db->facts) {
        return 0;
    }
    /* Without the job, `is_a`'s transitive closure was never materialized, so
     * the expansion would reach direct members only — a *partial* answer that
     * looks exactly like a narrow one, which is the ambiguity this feature is
     * meant to remove. Refused, the way `pattern` reports NOT_READY when
     * --no-fact-index takes its index away. */
    if (!db->config.inference) {
        return 1;
    }
    uint64_t *ids = NULL;
    size_t n = 0;
    pthread_rwlock_rdlock(&db->index_lock);
    int rc = fact_index_by_object(db->facts, FACT_OBJ_ID, p->pat_subject, NULL,
                                  "is_a", &ids, &n);
    pthread_rwlock_unlock(&db->index_lock);
    if (rc != 0) {
        return -1;
    }
    if (n == 0) {
        free(ids);
        return 0;
    }
    size_t cap = db->config.inference_max_subsume;
    if (cap && n > cap) {
        n = cap;
        p->subsume_truncated = 1;
    }
    uint64_t *subs = malloc(n * sizeof(*subs));
    if (!subs) {
        free(ids);
        return -1;
    }
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        MemoryRecord r;
        /* Scoped to the caller, not NULL. The fact indexes are server-wide, so
         * an unscoped expansion would let another tenant's taxonomy decide
         * which of *this* tenant's records answer — nothing of theirs leaks,
         * but the answer set would be chosen by data the caller cannot see. */
        if (qe_get(db, ids[i], p->agent_id, &r) != AEGIS_OK) {
            continue;
        }
        if (r.fact.kind == FACT_OBJ_ID && r.fact.subject != p->pat_subject) {
            subs[m++] = r.fact.subject;
        }
        record_free(&r);
    }
    free(ids);
    /* Deduplicated, which is not cosmetic: nothing stops two records asserting
     * the same `X is_a S` — the ordinary case is an agent writing it twice over
     * two sessions — and the union below issues one lookup per entry with no
     * dedup downstream, so a repeat would return every fact of X twice and
     * double the count. An earlier comment here claimed a duplicate cost only a
     * reload; the post-filter decides *whether* a candidate matches, never how
     * many times it appears. */
    qsort(subs, m, sizeof(*subs), cmp_u64_asc);
    size_t uniq = 0;
    for (size_t i = 0; i < m; i++) {
        if (i == 0 || subs[i] != subs[i - 1]) {
            subs[uniq++] = subs[i];
        }
    }
    p->subsume_ids = subs;
    p->subsume_n = uniq;
    return 0;
}

static aegis_status_t search_ex_body(AegisDB *db, const SearchParams *p,
                                     MemoryRecord **out_records,
                                     SearchExplain **out_explain,
                                     size_t *out_n) {
    aegis_status_t st = require_phase(db, p->embedding_dim ? 3 : 2);
    if (st != AEGIS_OK) {
        return st;
    }
    if (p->embedding_dim &&
        p->embedding_dim != db->config.embedding_dimensions) {
        return AEGIS_ERR_INVALID_REQUEST;
    }

    /* A text query needs the lexical index, which --no-lexical-index omits.
     * Report that as NOT_READY (the server cannot serve this yet) rather than
     * silently ignoring the query and returning unranked filter results. */
    if (p->query && *p->query && !db->lex) {
        return AEGIS_ERR_NOT_READY;
    }

    size_t top_k = p->top_k ? p->top_k : 10;
    int semantic = p->embedding_dim ? 1 : 0;
    int lexical = (p->query && *p->query) ? 1 : 0;
    CandSource src = CAND_FILTER;
    if (semantic && lexical) {
        src = CAND_HYBRID;
    } else if (semantic) {
        src = CAND_SEMANTIC;
    } else if (lexical) {
        src = CAND_LEXICAL;
    }
    int is_ranked = (src != CAND_FILTER);
    size_t offset = p->offset < MAX_OFFSET ? p->offset : MAX_OFFSET;
    /* rank enough to page past `offset` and still fill top_k */
    size_t want = offset + top_k;

    Cand *cands = NULL;
    size_t m = 0;
    if (is_ranked) {
        /* Over-fetch, then widen if a selective post-filter (or min_score, or a
         * page offset) leaves < want: the ranked sources return their best
         * matches regardless of filter, so a selective filter (e.g. a small
         * namespace) can drop them all. Re-query with a growing fetch until
         * enough survive, the source is exhausted, or the cap is hit. */
        size_t fetch = (want * 4) + 32;
        if (fetch > SEARCH_FETCH_CAP) {
            fetch = SEARCH_FETCH_CAP;
        }
        for (;;) {
            int exhausted = 0;
            st =
                gather_candidates(db, p, fetch, src, 0, &cands, &m, &exhausted);
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
        st = gather_candidates(db, p, 0, CAND_FILTER, db->config.query_scan_cap,
                               &cands, &m, &exhausted);
        if (st != AEGIS_OK) {
            return st;
        }
    }

    /* Rank the best `sel_n` (= offset + top_k, capped at m) into `ranked`
     * (sorted best-first), then page: return the slice [offset, sel_n) and free
     * the paged-over head. */
    int (*cmp)(const void *, const void *) =
        is_ranked ? cmp_score_desc : cmp_created_asc;
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
                /* Per hit, not per query: in a hybrid search one record may be
                 * found only lexically and the next only semantically, so these
                 * follow the ranks the sources actually assigned. */
                .semantic = ranked[i].srank ? 1 : 0,
                .lexical = ranked[i].lrank ? 1 : 0,
                .similarity = ranked[i].sim,
                .bm25 = ranked[i].bm25,
                .semantic_rank = (int)ranked[i].srank,
                .lexical_rank = (int)ranked[i].lrank,
                .rrf = ranked[i].rrf,
                .importance = ranked[i].rec.importance,
                .confidence = ranked[i].rec.confidence,
                .weight = ranked[i].weight,
                .recency_factor = ranked[i].recency,
                .score = ranked[i].score,
            };
        }
    }
    free(ranked);

    /* Usage feedback: the records actually handed back are the ones that were
     * recalled. Counted here rather than over the candidate set, because being a
     * candidate is not being used. The index read lock pins the table's structure
     * while the counters (atomics) are bumped; a concurrent writer growing the
     * table holds the write lock, so it cannot rehash underneath this. */
    if (p->track_usage && db->usage && rn) {
        uint64_t now = db_now_ms();
        pthread_rwlock_rdlock(&db->index_lock);
        for (size_t i = 0; i < rn; i++) {
            usage_index_record(db->usage, res[i].id, now);
        }
        pthread_rwlock_unlock(&db->index_lock);
    }

    *out_records = res;
    if (out_explain) {
        *out_explain = exp;
    }
    *out_n = rn;
    return AEGIS_OK;
}

/* Resolve the subsumption expansion once, run the search, release it. A
 * wrapper rather than an inline prologue because the body has several exits and
 * the expansion is the search's to own, not the caller's. */
aegis_status_t qe_search_ex(AegisDB *db, SearchParams *p,
                            MemoryRecord **out_records,
                            SearchExplain **out_explain, size_t *out_n) {
    SearchParams q = *p;
    int rs = resolve_subsume(db, &q);
    if (rs > 0) {
        return AEGIS_ERR_NOT_READY;
    }
    if (rs < 0) {
        return AEGIS_ERR_INTERNAL;
    }
    aegis_status_t st = search_ex_body(db, &q, out_records, out_explain, out_n);
    free((void *)q.subsume_ids);
    /* Reported back rather than swallowed: a caller that asked about a
     * category and silently got an answer over some of its members would have
     * no way to tell a narrow result from a truncated one. */
    p->subsume_truncated = q.subsume_truncated;
    return st;
}

/* Count live records matching the filters (type/tags/time/agent_id). Ignores
 * any embedding — count is over the filter predicate, not vector ranking. */
aegis_status_t qe_count(AegisDB *db, const SearchParams *p, size_t *out_count,
                        int *out_capped) {
    aegis_status_t st = require_phase(db, 2);
    if (st != AEGIS_OK) {
        return st;
    }
    /* `count` takes `pattern`, so it takes `subsume` with it — "how many
     * records assert this?" should mean the same thing under both ops. */
    SearchParams q = *p;
    int rs = resolve_subsume(db, &q);
    if (rs > 0) {
        return AEGIS_ERR_NOT_READY;
    }
    if (rs < 0) {
        return AEGIS_ERR_INTERNAL;
    }
    p = &q;
    Cand *cands = NULL;
    size_t m = 0;
    int exhausted = 0;
    st = gather_candidates(db, p, 0, 0, db->config.query_scan_cap, &cands, &m,
                           &exhausted);
    free((void *)q.subsume_ids);
    q.subsume_ids = NULL;
    /* Folded into `capped`, which already means "this number is over a
     * bounded view". A count computed across a truncated expansion is exactly
     * that, and leaving it unsaid would be the narrow-versus-partial ambiguity
     * `search` reports with subsume_truncated. */
    int sub_trunc = q.subsume_truncated;
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
        *out_capped = !exhausted || sub_trunc;
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
