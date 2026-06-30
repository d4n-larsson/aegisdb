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

#include "aegisdb/hnsw.h"

#define DEFAULT_ANN_THRESHOLD 10000 /* live vectors above which HNSW kicks in */

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

struct SemanticIndex {
    size_t dim;
    SemEntry *e;
    size_t n;
    size_t cap;
    MapSlot *map; /* id -> dense slot; power-of-two capacity, NULL until first add */
    size_t mcap;
    Hnsw *hnsw;          /* NULL until the live count crosses ann_threshold */
    size_t ann_threshold;
    size_t ef_search;    /* HNSW query beam width; 0 = the HNSW default */
};

#define MAP_INITIAL_CAP 128 /* power of two; covers the dense array's first growths */

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

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

SemanticIndex *semantic_index_create(size_t dim, size_t ann_threshold,
                                     size_t ef_search) {
    SemanticIndex *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->dim = dim;
    s->ann_threshold = ann_threshold ? ann_threshold : DEFAULT_ANN_THRESHOLD;
    s->ef_search = ef_search;
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
    hnsw_free(s->hnsw);
    s->hnsw = NULL;
}

/* Build the HNSW graph from the current dense array (one-time, at the threshold
 * crossing), then drop the dense array so the graph holds the only copy of each
 * vector; thereafter add/remove operate on the graph. Returns 0/-1. */
static int build_hnsw(SemanticIndex *s) {
    HnswParams p = {.ef_search = s->ef_search, .seed = 0x9E3779B97F4A7C15ULL};
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

static float l2norm(const float *v, size_t dim) {
    double acc = 0;
    for (size_t i = 0; i < dim; i++) acc += (double)v[i] * v[i];
    return (float)sqrt(acc);
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

int semantic_index_add(SemanticIndex *s, uint64_t id, const float *vec,
                       size_t dim) {
    if (dim != s->dim) return -1;
    /* Above the threshold the graph is authoritative (dense array is gone). */
    if (s->hnsw) return hnsw_add(s->hnsw, id, vec, dim) == 0 ? 0 : -1;

    int rv = dense_put(s, id, vec, dim);
    if (rv < 0) return -1;
    if (rv == 1 && s->n >= s->ann_threshold)
        return build_hnsw(s); /* crossed the threshold: build graph, drop dense */
    return 0;
}

void semantic_index_remove(SemanticIndex *s, uint64_t id) {
    if (s->hnsw) {
        hnsw_remove(s->hnsw, id);
        return;
    }
    if (s->mcap == 0) return;
    size_t j = map_probe(s->map, s->mcap, id);
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

int semantic_index_search(const SemanticIndex *s, const float *query,
                          size_t dim, size_t top_k, uint64_t **out_ids,
                          float **out_scores, size_t *out_n) {
    if (dim != s->dim) return -1;
    /* Above the threshold the graph is authoritative; the dense array is gone.
     * top_k==0 means "all", which the graph serves by querying for its live
     * count. */
    if (s->hnsw) {
        size_t k = top_k ? top_k : hnsw_count(s->hnsw);
        if (k == 0) {
            *out_ids = malloc(sizeof(uint64_t));
            *out_scores = malloc(sizeof(float));
            *out_n = 0;
            return (*out_ids && *out_scores) ? 0 : -1;
        }
        return hnsw_search(s->hnsw, query, dim, k, s->ef_search, out_ids,
                           out_scores, out_n);
    }
    float qnorm = l2norm(query, dim);
    Scored *all = malloc((s->n ? s->n : 1) * sizeof(Scored));
    if (!all) return -1;
    size_t m = 0;
    for (size_t i = 0; i < s->n; i++) {
        if (!s->e[i].used) continue;
        double dot = 0;
        const float *v = s->e[i].vec;
        for (size_t k = 0; k < dim; k++) dot += (double)v[k] * query[k];
        float denom = s->e[i].norm * qnorm;
        float sim = denom > 0 ? (float)(dot / denom) : 0.0f;
        all[m].id = s->e[i].id;
        all[m].score = sim;
        m++;
    }
    size_t k = (top_k && top_k < m) ? top_k : m;
    /* Select the k highest scores in O(m log k): keep a size-k min-heap of the
     * best seen, then sort just those k. Falls back to a full sort when k == m. */
    if (k < m) {
        for (size_t i = k / 2; i-- > 0;) scored_sift_down(all, k, i);
        for (size_t i = k; i < m; i++)
            if (all[i].score > all[0].score) {
                all[0] = all[i];
                scored_sift_down(all, k, 0);
            }
    }
    qsort(all, k, sizeof(Scored), cmp_scored_desc);
    uint64_t *ids = malloc((k ? k : 1) * sizeof(uint64_t));
    float *sc = malloc((k ? k : 1) * sizeof(float));
    if (!ids || !sc) {
        free(ids);
        free(sc);
        free(all);
        return -1;
    }
    for (size_t i = 0; i < k; i++) {
        ids[i] = all[i].id;
        sc[i] = all[i].score;
    }
    free(all);
    *out_ids = ids;
    *out_scores = sc;
    *out_n = k;
    return 0;
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

int semantic_index_load(SemanticIndex *s, const char *path,
                        uint64_t *out_covered_log_size) {
    if (s->hnsw || s->n != 0) return -1; /* must be a fresh index */
    Hnsw *g = hnsw_load(path, s->dim, out_covered_log_size);
    if (!g) return -1;
    s->hnsw = g; /* the graph is authoritative; no dense array to rebuild */
    return 0;
}

/* Gather every live id (from the graph if present, else the dense array) into a
 * growing list. */
struct id_list {
    uint64_t *ids;
    size_t n, cap;
    int err;
};
static int collect_id(uint64_t id, const float *vec, void *ctx) {
    (void)vec;
    struct id_list *l = ctx;
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 16;
        uint64_t *g = realloc(l->ids, nc * sizeof(uint64_t));
        if (!g) { l->err = 1; return 1; }
        l->ids = g;
        l->cap = nc;
    }
    l->ids[l->n++] = id;
    return 0;
}

void semantic_index_reconcile(SemanticIndex *s,
                              int (*keep)(uint64_t id, void *ctx), void *ctx) {
    struct id_list l = {0};
    if (s->hnsw) {
        hnsw_foreach_live(s->hnsw, collect_id, &l);
    } else {
        for (size_t i = 0; i < s->n && !l.err; i++) collect_id(s->e[i].id, NULL, &l);
    }
    /* remove after gathering so we don't disturb the structure being iterated */
    for (size_t i = 0; i < l.n; i++)
        if (!keep(l.ids[i], ctx)) semantic_index_remove(s, l.ids[i]);
    free(l.ids);
}
