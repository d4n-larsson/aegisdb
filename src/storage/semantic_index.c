/* Semantic index (T035, #38): hybrid cosine top-K over stored float32 vectors.
 *
 * Vectors live in a dense array `e[0..n)`; an open-addressing `id -> slot` map
 * keeps add / lookup / remove O(1) so building or recovering an index of N
 * vectors is O(N), not O(N^2). The map stores the dense index, so a swap-remove
 * only has to repoint the one moved entry.
 *
 * Search is exact (brute-force scan over the dense array) while small. Once the
 * live count crosses `ann_threshold`, an HNSW graph is built from the dense
 * array and the dense array is freed: the graph then becomes the sole store
 * (one copy of each vector) and serves add/remove/search/count. The switch is
 * one-way — a later dip below the threshold keeps using the graph. */
#include "aegisdb/semantic_index.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/hash_mix.h"
#include "aegisdb/hnsw.h"
#include "aegisdb/vecmath.h"

#define DEFAULT_ANN_THRESHOLD 10000 /* live vectors above which HNSW kicks in */

/* Multi-vector (#85): a record's N vectors are stored as N separate entries
 * keyed by a synthetic id `record << MV_SLOT_BITS | slot`, so the dense map and
 * HNSW (both keyed by an opaque uint64) stay 1:1 per vector. Search maps the
 * synthetic ids back to record ids and returns each record once (best-of-N).
 * MV_SLOT_BITS caps a record at 64 vectors (matches the wire cap). */
#define MV_SLOT_BITS 6
#define MV_SLOT_MASK ((1u << MV_SLOT_BITS) - 1u)
static uint64_t mv_syn(uint64_t rec, size_t slot) {
    return (rec << MV_SLOT_BITS) | (uint64_t)slot;
}
static uint64_t mv_rec(uint64_t syn) { return syn >> MV_SLOT_BITS; }

typedef struct {
    uint64_t id;
    float *vec;
    float norm; /* precomputed L2 norm */
    int used;
} SemEntry;

typedef struct {
    uint64_t id;
    size_t idx; /* position of this id in SemanticIndex.e */
    int used;
} MapSlot;

/* record id -> number of vectors it currently has (so remove/replace know how
 * many synthetic slots to drop). Open addressing with backward-shift deletion. */
typedef struct {
    uint64_t rec;
    uint32_t count;
    int used;
} RCount;

struct SemanticIndex {
    size_t dim;
    SemEntry *e;
    size_t n;
    size_t cap;
    MapSlot *map; /* id -> dense slot; power-of-two capacity, NULL until first add */
    size_t mcap;
    RCount *rc;   /* record -> vec_count; power-of-two capacity */
    size_t rccap, rcn;
    Hnsw *hnsw;          /* NULL until the live count crosses ann_threshold */
    size_t ann_threshold;
    size_t ef_search;    /* HNSW query beam width; 0 = the HNSW default */
    int quantize;        /* store HNSW vectors as int8 (#75) */
};

#define MAP_INITIAL_CAP 128 /* power of two; covers the dense array's first growths */

/* Probe to the slot holding `id`, or the empty slot where it would go. Requires
 * mcap > 0 and at least one empty slot (the load factor guarantees both). */
static size_t map_probe(const MapSlot *map, size_t mcap, uint64_t id) {
    size_t mask = mcap - 1;
    size_t i = (size_t)mix64(id) & mask;
    while (map[i].used && map[i].id != id) i = (i + 1) & mask;
    return i;
}

/* Rebuild the map at `newcap` from the dense array (the dense indices are the
 * source of truth, so this also resolves any prior probe-chain layout). */
static int map_grow(SemanticIndex *s, size_t newcap) {
    MapSlot *nm = calloc(newcap, sizeof(MapSlot));
    if (!nm) return -1;
    size_t mask = newcap - 1;
    for (size_t i = 0; i < s->n; i++) {
        size_t j = (size_t)mix64(s->e[i].id) & mask;
        while (nm[j].used) j = (j + 1) & mask;
        nm[j].used = 1;
        nm[j].id = s->e[i].id;
        nm[j].idx = i;
    }
    free(s->map);
    s->map = nm;
    s->mcap = newcap;
    return 0;
}

/* Backward-shift deletion: remove the entry at slot `i`, sliding later members
 * of its probe chain back so no tombstones are left and probes stay correct. */
static void map_delete_at(SemanticIndex *s, size_t i) {
    MapSlot *m = s->map;
    size_t mask = s->mcap - 1;
    size_t j = i;
    for (;;) {
        m[i].used = 0;
        size_t k;
        do {
            j = (j + 1) & mask;
            if (!m[j].used) return;
            k = (size_t)mix64(m[j].id) & mask; /* home slot of m[j] */
        } while (i <= j ? (i < k && k <= j) : (i < k || k <= j));
        m[i] = m[j]; /* moves id+idx together; used stays 1 */
        i = j;
    }
}

/* ----- record -> vec_count map (rc) ------------------------------------- */
static size_t rc_probe(const RCount *m, size_t cap, uint64_t rec) {
    size_t mask = cap - 1, i = (size_t)mix64(rec) & mask;
    while (m[i].used && m[i].rec != rec) i = (i + 1) & mask;
    return i;
}
static int rc_grow(SemanticIndex *s, size_t newcap) {
    RCount *nm = calloc(newcap, sizeof(RCount));
    if (!nm) return -1;
    size_t mask = newcap - 1;
    for (size_t i = 0; i < s->rccap; i++) {
        if (!s->rc[i].used) continue;
        size_t j = (size_t)mix64(s->rc[i].rec) & mask;
        while (nm[j].used) j = (j + 1) & mask;
        nm[j] = s->rc[i];
    }
    free(s->rc);
    s->rc = nm;
    s->rccap = newcap;
    return 0;
}
static uint32_t rc_get(const SemanticIndex *s, uint64_t rec) {
    if (s->rccap == 0) return 0;
    size_t i = rc_probe(s->rc, s->rccap, rec);
    return s->rc[i].used ? s->rc[i].count : 0;
}
static int rc_set(SemanticIndex *s, uint64_t rec, uint32_t count) {
    if (s->rccap == 0 && rc_grow(s, 64) != 0) return -1;
    size_t i = rc_probe(s->rc, s->rccap, rec);
    if (!s->rc[i].used) {
        if ((s->rcn + 1) * 10 > s->rccap * 7) {
            if (rc_grow(s, s->rccap * 2) != 0) return -1;
            i = rc_probe(s->rc, s->rccap, rec);
        }
        s->rc[i].used = 1;
        s->rc[i].rec = rec;
        s->rcn++;
    }
    s->rc[i].count = count;
    return 0;
}
static void rc_del(SemanticIndex *s, uint64_t rec) {
    if (s->rccap == 0) return;
    size_t mask = s->rccap - 1;
    size_t i = rc_probe(s->rc, s->rccap, rec);
    if (!s->rc[i].used) return;
    s->rc[i].used = 0;
    s->rcn--;
    for (size_t j = (i + 1) & mask; s->rc[j].used; j = (j + 1) & mask) {
        size_t home = (size_t)mix64(s->rc[j].rec) & mask;
        if (i <= j ? (i < home && home <= j) : (i < home || home <= j)) continue;
        s->rc[i] = s->rc[j];
        s->rc[j].used = 0;
        i = j;
    }
}

SemanticIndex *semantic_index_create(size_t dim, size_t ann_threshold,
                                     size_t ef_search, int quantize) {
    SemanticIndex *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->dim = dim;
    s->ann_threshold = ann_threshold ? ann_threshold : DEFAULT_ANN_THRESHOLD;
    s->ef_search = ef_search;
    s->quantize = quantize;
    return s;
}

size_t semantic_index_count(const SemanticIndex *s) {
    if (!s) return 0;
    /* Above the threshold the graph is authoritative; the dense array is freed. */
    return s->hnsw ? hnsw_count(s->hnsw) : s->n;
}

/* Free the dense array + id-map, keeping the HNSW graph. Used once the graph
 * becomes authoritative so each vector is stored once (in the graph). */
static void drop_dense(SemanticIndex *s) {
    for (size_t i = 0; i < s->n; i++) free(s->e[i].vec);
    free(s->e);
    s->e = NULL;
    s->n = s->cap = 0;
    free(s->map);
    s->map = NULL;
    s->mcap = 0;
}

void semantic_index_free(SemanticIndex *s) {
    if (!s) return;
    semantic_index_clear(s);
    free(s);
}

/* Reset to empty, preserving dim/ann_threshold/ef_search. */
void semantic_index_clear(SemanticIndex *s) {
    if (!s) return;
    for (size_t i = 0; i < s->n; i++) free(s->e[i].vec);
    free(s->e);
    s->e = NULL;
    s->n = s->cap = 0;
    free(s->map);
    s->map = NULL;
    s->mcap = 0;
    free(s->rc);
    s->rc = NULL;
    s->rccap = s->rcn = 0;
    hnsw_free(s->hnsw);
    s->hnsw = NULL;
}

/* Build the HNSW graph from the current dense array (one-time, at the threshold
 * crossing), then drop the dense array so the graph holds the only copy of each
 * vector; thereafter add/remove operate on the graph. Returns 0/-1. */
static int build_hnsw(SemanticIndex *s) {
    HnswParams p = {.ef_search = s->ef_search, .seed = 0x9E3779B97F4A7C15ULL,
                    .quantize = s->quantize};
    Hnsw *h = hnsw_create(s->dim, &p);
    if (!h) return -1;
    for (size_t i = 0; i < s->n; i++) {
        if (hnsw_add(h, s->e[i].id, s->e[i].vec, s->dim) != 0) {
            hnsw_free(h);
            return -1;
        }
    }
    s->hnsw = h;
    drop_dense(s);
    return 0;
}

/* Insert or replace id's vector in the dense array + id-map only (no graph).
 * Returns 1 if a new entry was added, 0 if an existing one was replaced in
 * place, -1 on allocation failure. */
static int dense_put(SemanticIndex *s, uint64_t id, const float *vec,
                     size_t dim) {
    if (s->mcap == 0 && map_grow(s, MAP_INITIAL_CAP) != 0) return -1;

    size_t j = map_probe(s->map, s->mcap, id);
    if (s->map[j].used) {
        SemEntry *e = &s->e[s->map[j].idx];
        float *nv = realloc(e->vec, dim * sizeof(float));
        if (!nv) return -1;
        memcpy(nv, vec, dim * sizeof(float));
        e->vec = nv;
        e->norm = l2norm(vec, dim);
        return 0;
    }

    /* New id. Copy the vector first so an allocation failure changes nothing. */
    float *nv = malloc(dim * sizeof(float));
    if (!nv) return -1;
    memcpy(nv, vec, dim * sizeof(float));

    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        SemEntry *ne = realloc(s->e, cap * sizeof(SemEntry));
        if (!ne) {
            free(nv);
            return -1;
        }
        s->e = ne;
        s->cap = cap;
    }
    /* Keep the map under a ~0.7 load factor; re-probe after a resize. */
    if ((s->n + 1) * 10 > s->mcap * 7) {
        if (map_grow(s, s->mcap * 2) != 0) {
            free(nv);
            return -1;
        }
        j = map_probe(s->map, s->mcap, id);
    }

    size_t idx = s->n++;
    s->e[idx].id = id;
    s->e[idx].vec = nv;
    s->e[idx].norm = l2norm(vec, dim);
    s->e[idx].used = 1;
    s->map[j].used = 1;
    s->map[j].id = id;
    s->map[j].idx = idx;
    return 1;
}

/* Remove one synthetic-id vector from the dense array (caller ensures no HNSW). */
static void dense_remove(SemanticIndex *s, uint64_t synid) {
    if (s->mcap == 0) return;
    size_t j = map_probe(s->map, s->mcap, synid);
    if (!s->map[j].used) return;
    size_t idx = s->map[j].idx;
    map_delete_at(s, j);

    free(s->e[idx].vec);
    /* swap-remove: move the last entry into the hole so the array stays dense
     * and search stays O(live entries); repoint the moved id's map slot. */
    size_t last = s->n - 1;
    if (idx != last) {
        s->e[idx] = s->e[last];
        size_t mj = map_probe(s->map, s->mcap, s->e[idx].id);
        s->map[mj].idx = idx;
    }
    s->n--;
}

/* Add/remove one vector by synthetic id, routing to the graph or dense array. */
static int vec_add(SemanticIndex *s, uint64_t synid, const float *vec) {
    if (s->hnsw) return hnsw_add(s->hnsw, synid, vec, s->dim) == 0 ? 0 : -1;
    return dense_put(s, synid, vec, s->dim) < 0 ? -1 : 0;
}
static void vec_remove(SemanticIndex *s, uint64_t synid) {
    if (s->hnsw)
        hnsw_remove(s->hnsw, synid);
    else
        dense_remove(s, synid);
}

int semantic_index_add(SemanticIndex *s, uint64_t id, const float *vecs,
                       size_t vec_count, size_t dim) {
    if (dim != s->dim || vec_count == 0 || vec_count > (MV_SLOT_MASK + 1u))
        return -1;
    /* replace: drop the record's existing vectors (its prior slots) */
    uint32_t old = rc_get(s, id);
    for (uint32_t slot = 0; slot < old; slot++) vec_remove(s, mv_syn(id, slot));

    for (size_t slot = 0; slot < vec_count; slot++)
        if (vec_add(s, mv_syn(id, slot), vecs + slot * dim) != 0) return -1;
    if (rc_set(s, id, (uint32_t)vec_count) != 0) return -1;

    /* crossed the threshold: build the graph from the dense array, drop dense */
    if (!s->hnsw && s->n >= s->ann_threshold) return build_hnsw(s);
    return 0;
}

void semantic_index_remove(SemanticIndex *s, uint64_t id) {
    uint32_t old = rc_get(s, id);
    for (uint32_t slot = 0; slot < old; slot++) vec_remove(s, mv_syn(id, slot));
    rc_del(s, id);
}

typedef struct {
    uint64_t id;
    float score;
} Scored;

static int cmp_scored_desc(const void *a, const void *b) {
    float fa = ((const Scored *)a)->score;
    float fb = ((const Scored *)b)->score;
    if (fa < fb) return 1;
    if (fa > fb) return -1;
    return 0;
}

/* Sift element i down a heap of size n ordered so the root is the LOWEST score
 * (the next to evict): a size-k heap built this way retains the k top scores. */
static void scored_sift_down(Scored *a, size_t n, size_t i) {
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, low = i;
        if (l < n && a[l].score < a[low].score) low = l;
        if (r < n && a[r].score < a[low].score) low = r;
        if (low == i) break;
        Scored t = a[i];
        a[i] = a[low];
        a[low] = t;
        i = low;
    }
}

/* Collapse (record, score) candidates (which may repeat a record across its
 * vectors) to the top_k distinct records by best score. `cand[i].id` is a
 * record id. top_k == 0 returns all distinct records. Allocates the outputs. */
typedef struct { uint64_t rec; size_t idx; int used; } DedupSlot;
static int dedup_topk(Scored *cand, size_t m, size_t top_k, uint64_t **out_ids,
                      float **out_scores, size_t *out_n) {
    size_t cap = 16;
    while (cap < m * 2) cap *= 2;
    DedupSlot *ds = calloc(cap, sizeof(DedupSlot));
    Scored *uni = malloc((m ? m : 1) * sizeof(Scored));
    if (!ds || !uni) {
        free(ds);
        free(uni);
        return -1;
    }
    size_t un = 0, mask = cap - 1;
    for (size_t i = 0; i < m; i++) {
        uint64_t rec = cand[i].id;
        size_t h = mix64(rec) & mask;
        while (ds[h].used && ds[h].rec != rec) h = (h + 1) & mask;
        if (ds[h].used) {
            if (cand[i].score > uni[ds[h].idx].score)
                uni[ds[h].idx].score = cand[i].score; /* best-of-N */
        } else {
            ds[h].used = 1;
            ds[h].rec = rec;
            ds[h].idx = un;
            uni[un].id = rec;
            uni[un].score = cand[i].score;
            un++;
        }
    }
    free(ds);

    size_t k = (top_k && top_k < un) ? top_k : un;
    if (k < un) {
        for (size_t i = k / 2; i-- > 0;) scored_sift_down(uni, k, i);
        for (size_t i = k; i < un; i++)
            if (uni[i].score > uni[0].score) {
                uni[0] = uni[i];
                scored_sift_down(uni, k, 0);
            }
    }
    qsort(uni, k, sizeof(Scored), cmp_scored_desc);
    uint64_t *ids = malloc((k ? k : 1) * sizeof(uint64_t));
    float *sc = malloc((k ? k : 1) * sizeof(float));
    if (!ids || !sc) {
        free(ids);
        free(sc);
        free(uni);
        return -1;
    }
    for (size_t i = 0; i < k; i++) {
        ids[i] = uni[i].id;
        sc[i] = uni[i].score;
    }
    free(uni);
    *out_ids = ids;
    *out_scores = sc;
    *out_n = k;
    return 0;
}

int semantic_index_search(const SemanticIndex *s, const float *query,
                          size_t dim, size_t top_k, uint64_t **out_ids,
                          float **out_scores, size_t *out_n) {
    if (dim != s->dim) return -1;

    if (s->hnsw) {
        /* Over-fetch synthetic (per-vector) candidates so record-dedup still
         * leaves enough distinct records; then collapse best-of-N. */
        size_t want = top_k ? top_k : hnsw_count(s->hnsw);
        if (want == 0) {
            *out_ids = malloc(sizeof(uint64_t));
            *out_scores = malloc(sizeof(float));
            *out_n = 0;
            return (*out_ids && *out_scores) ? 0 : -1;
        }
        size_t live = hnsw_count(s->hnsw);
        size_t fetch = want * 2;
        if (fetch > live) fetch = live;
        if (fetch < want) fetch = want;
        uint64_t *sid = NULL;
        float *ssc = NULL;
        size_t sn = 0;
        if (hnsw_search(s->hnsw, query, dim, fetch, s->ef_search, &sid, &ssc,
                        &sn) != 0)
            return -1;
        Scored *cand = malloc((sn ? sn : 1) * sizeof(Scored));
        if (!cand) {
            free(sid);
            free(ssc);
            return -1;
        }
        for (size_t i = 0; i < sn; i++) {
            cand[i].id = mv_rec(sid[i]);
            cand[i].score = ssc[i];
        }
        free(sid);
        free(ssc);
        int rc = dedup_topk(cand, sn, top_k, out_ids, out_scores, out_n);
        free(cand);
        return rc;
    }

    /* dense/exact: score every stored vector, then collapse best-of-N */
    float qnorm = l2norm(query, dim);
    Scored *cand = malloc((s->n ? s->n : 1) * sizeof(Scored));
    if (!cand) return -1;
    size_t m = 0;
    for (size_t i = 0; i < s->n; i++) {
        if (!s->e[i].used) continue;
        float dot = dot_f32(s->e[i].vec, query, dim);
        float denom = s->e[i].norm * qnorm;
        cand[m].id = mv_rec(s->e[i].id);
        cand[m].score = denom > 0 ? (float)(dot / denom) : 0.0f;
        m++;
    }
    int rc = dedup_topk(cand, m, top_k, out_ids, out_scores, out_n);
    free(cand);
    return rc;
}
/* ----------------------------------------------------------- persistence -- */

int semantic_index_save(const SemanticIndex *s, const char *path,
                        uint64_t covered_log_size) {
    if (!s->hnsw) {
        /* below the threshold there is no graph; drop any stale checkpoint so
         * recovery rebuilds from the log rather than trusting it */
        unlink(path);
        return 0;
    }
    return hnsw_save(s->hnsw, path, covered_log_size);
}

typedef struct {
    SemanticIndex *s;
    int err;
} RcRebuild;
/* Called per live graph node on load: bump the record's vec_count to cover this
 * slot (slots are contiguous 0..count-1, so max slot + 1 is the count). */
static int rc_rebuild_cb(uint64_t synid, const float *vec, void *ctx) {
    (void)vec;
    RcRebuild *c = ctx;
    uint64_t rec = mv_rec(synid);
    uint32_t slot = (uint32_t)(synid & MV_SLOT_MASK);
    uint32_t cur = rc_get(c->s, rec);
    if (slot + 1 > cur && rc_set(c->s, rec, slot + 1) != 0) {
        c->err = 1;
        return 1;
    }
    return 0;
}

int semantic_index_load(SemanticIndex *s, const char *path,
                        uint64_t *out_covered_log_size) {
    if (s->hnsw || s->n != 0) return -1; /* must be a fresh index */
    Hnsw *g = hnsw_load(path, s->dim, out_covered_log_size);
    if (!g) return -1;
    /* A checkpoint saved in a different quantization mode than we're configured
     * for is rejected, so recovery rebuilds it in the configured mode. */
    if (hnsw_is_quantized(g) != (s->quantize ? 1 : 0)) {
        hnsw_free(g);
        return -1;
    }
    s->hnsw = g; /* the graph is authoritative; no dense array to rebuild */
    /* Rebuild the record->vec_count map from the graph's synthetic node ids
     * (record = id >> MV_SLOT_BITS; slot = id & mask); count = max slot + 1. */
    RcRebuild cx = {s, 0};
    hnsw_foreach_live(g, rc_rebuild_cb, &cx);
    if (cx.err) {
        semantic_index_clear(s);
        return -1;
    }
    return 0;
}

void semantic_index_reconcile(SemanticIndex *s,
                              int (*keep)(uint64_t id, void *ctx), void *ctx) {
    /* The rc map holds exactly the live record ids (one entry per record,
     * regardless of vector count). Snapshot them, then remove those not kept —
     * gathering first so removal doesn't disturb the map we're iterating. */
    uint64_t *recs = malloc((s->rcn ? s->rcn : 1) * sizeof(uint64_t));
    if (!recs) return;
    size_t n = 0;
    for (size_t i = 0; i < s->rccap; i++)
        if (s->rc[i].used) recs[n++] = s->rc[i].rec;
    for (size_t i = 0; i < n; i++)
        if (!keep(recs[i], ctx)) semantic_index_remove(s, recs[i]);
    free(recs);
}
