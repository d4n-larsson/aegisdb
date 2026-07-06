/* HNSW approximate nearest-neighbour index over float32 vectors in cosine
 * space (#38). Self-contained C17. See include/aegisdb/hnsw.h.
 *
 * Layout: nodes live in a dense array; a node index is a uint32. Each node has
 * a per-layer neighbour list (capacity M0 at layer 0, M above). An id->node map
 * (open addressing) supports replace/remove. Distance is 1 - cosine_similarity
 * (so "nearest" == smallest distance); vectors are stored with a precomputed L2
 * norm. Deletes are soft (tombstone): the node stays in the graph for
 * connectivity but is never returned and never linked to by new inserts. */
#include "aegisdb/hnsw.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/crc32.h"
#include "aegisdb/hash_mix.h"
#include "aegisdb/vecmath.h"

#define HNSW_FORMAT_MAGIC "AHNS"
#define HNSW_FORMAT_VERSION 2u /* v2 adds the quantized flag + int8 vector layout */

#define NPOS UINT32_MAX

/* ------------------------------------------------------------------ PRNG -- */
static uint64_t xorshift(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

/* ----------------------------------------------------------------- nodes -- */
typedef struct {
    uint64_t id;
    float *vec;       /* float mode: dim floats; quant mode: NULL */
    int8_t *qvec;     /* quant mode: dim int8 (v[i] ~= qvec[i]*scale); else NULL */
    float scale;      /* quant mode: dequant scale */
    float norm;       /* precomputed L2 norm (of the stored representation) */
    int deleted;      /* tombstone: excluded from results, kept for connectivity */
    int top_layer;    /* highest layer this node belongs to */
    uint32_t **links; /* links[l] = neighbour node indices at layer l */
    uint32_t *link_cnt;
} Node;

typedef struct {
    uint64_t id;
    uint32_t node;
    int used;
} MapSlot;

typedef struct {
    float d;
    uint32_t node;
} Cand;

struct Hnsw {
    size_t dim, M, M0, ef_construction, ef_search;
    int quantized;   /* store vectors as int8 (see #75) */
    double mL;       /* level-generation normalization = 1/ln(M) */
    uint64_t rng;

    Node *nodes;
    size_t n, cap;   /* total nodes incl. tombstoned */
    size_t live;     /* non-tombstoned */
    uint32_t entry;  /* entry-point node, or NPOS when empty */
    int max_layer;

    MapSlot *map;    /* id -> node index; power-of-two capacity */
    size_t mcap;

    /* reusable scratch for neighbour pruning (insert is single-threaded under
     * the caller's write lock): scratch_cand sized M0+1, scratch_sel sized M0 */
    Cand *scratch_cand;
    uint32_t *scratch_sel;
};

/* ------------------------------------------------------------- id -> node -- */
static size_t map_probe(const MapSlot *m, size_t mcap, uint64_t id) {
    size_t mask = mcap - 1, i = (size_t)mix64(id) & mask;
    while (m[i].used && m[i].id != id) i = (i + 1) & mask;
    return i;
}

static int map_grow(Hnsw *h, size_t newcap) {
    MapSlot *nm = calloc(newcap, sizeof(MapSlot));
    if (!nm) return -1;
    size_t mask = newcap - 1;
    for (size_t i = 0; i < h->mcap; i++) {
        if (!h->map[i].used) continue;
        size_t j = (size_t)mix64(h->map[i].id) & mask;
        while (nm[j].used) j = (j + 1) & mask;
        nm[j] = h->map[i];
    }
    free(h->map);
    h->map = nm;
    h->mcap = newcap;
    return 0;
}

static uint32_t map_get(const Hnsw *h, uint64_t id) {
    if (h->mcap == 0) return NPOS;
    size_t i = map_probe(h->map, h->mcap, id);
    return h->map[i].used ? h->map[i].node : NPOS;
}

static int map_put(Hnsw *h, uint64_t id, uint32_t node) {
    if (h->mcap == 0 && map_grow(h, 64) != 0) return -1;
    size_t i = map_probe(h->map, h->mcap, id);
    if (!h->map[i].used) {
        /* count live map entries against the load factor */
        if ((h->live + 1) * 10 > h->mcap * 7) {
            if (map_grow(h, h->mcap * 2) != 0) return -1;
            i = map_probe(h->map, h->mcap, id);
        }
        h->map[i].used = 1;
        h->map[i].id = id;
    }
    h->map[i].node = node;
    return 0;
}

static void map_del(Hnsw *h, uint64_t id) {
    if (h->mcap == 0) return;
    size_t mask = h->mcap - 1;
    size_t i = map_probe(h->map, h->mcap, id);
    if (!h->map[i].used) return;
    h->map[i].used = 0;
    size_t j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (!h->map[j].used) break;
        size_t k = (size_t)mix64(h->map[j].id) & mask;
        if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) continue;
        h->map[i] = h->map[j];
        h->map[j].used = 0;
        i = j;
    }
}

/* -------------------------------------------------------------- distance -- */
/* Dot product of a float query vector with node `nd`'s stored vector, handling
 * either storage mode (float, or int8 dequantized via the node's scale). */
static double node_dot_query(const Hnsw *h, const float *q, const Node *nd) {
    double dot = 0;
    if (h->quantized) {
        const int8_t *v = nd->qvec;
        for (size_t i = 0; i < h->dim; i++) dot += (double)q[i] * v[i];
        return dot * nd->scale;
    }
    const float *v = nd->vec;
    for (size_t i = 0; i < h->dim; i++) dot += (double)q[i] * v[i];
    return dot;
}

/* Dot product of two stored node vectors. */
static double node_dot_node(const Hnsw *h, const Node *a, const Node *b) {
    double dot = 0;
    if (h->quantized) {
        const int8_t *va = a->qvec, *vb = b->qvec;
        long acc = 0;
        for (size_t i = 0; i < h->dim; i++) acc += (long)va[i] * vb[i];
        return (double)acc * a->scale * b->scale;
    }
    const float *va = a->vec, *vb = b->vec;
    for (size_t i = 0; i < h->dim; i++) dot += (double)va[i] * vb[i];
    return dot;
}

/* 1 - cosine similarity, in [0, 2]; smaller is nearer. */
static float dist_q(const Hnsw *h, const float *q, float qnorm, uint32_t node) {
    const Node *nd = &h->nodes[node];
    double dot = node_dot_query(h, q, nd);
    float denom = qnorm * nd->norm;
    return denom > 0 ? 1.0f - (float)(dot / denom) : 1.0f;
}

/* ------------------------------------------------------------ candidate heaps */
typedef struct {
    Cand *a;
    size_t n, cap;
} Heap;

static int heap_reserve(Heap *h, size_t want) {
    if (want <= h->cap) return 0;
    size_t nc = h->cap ? h->cap * 2 : 16;
    while (nc < want) nc *= 2;
    Cand *na = realloc(h->a, nc * sizeof(Cand));
    if (!na) return -1;
    h->a = na;
    h->cap = nc;
    return 0;
}

/* maxh: parent has larger d (root = farthest). !maxh: root = nearest. */
static void heap_up(Cand *a, size_t i, int maxh) {
    while (i) {
        size_t p = (i - 1) / 2;
        int up = maxh ? (a[i].d > a[p].d) : (a[i].d < a[p].d);
        if (!up) break;
        Cand t = a[i];
        a[i] = a[p];
        a[p] = t;
        i = p;
    }
}
static void heap_down(Cand *a, size_t n, size_t i, int maxh) {
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, b = i;
        if (l < n && (maxh ? a[l].d > a[b].d : a[l].d < a[b].d)) b = l;
        if (r < n && (maxh ? a[r].d > a[b].d : a[r].d < a[b].d)) b = r;
        if (b == i) break;
        Cand t = a[i];
        a[i] = a[b];
        a[b] = t;
        i = b;
    }
}
static int heap_push(Heap *h, Cand v, int maxh) {
    if (heap_reserve(h, h->n + 1) != 0) return -1;
    h->a[h->n] = v;
    heap_up(h->a, h->n, maxh);
    h->n++;
    return 0;
}
static Cand heap_pop(Heap *h, int maxh) {
    Cand top = h->a[0];
    h->a[0] = h->a[--h->n];
    heap_down(h->a, h->n, 0, maxh);
    return top;
}

static int cmp_cand_asc(const void *a, const void *b) {
    float x = ((const Cand *)a)->d, y = ((const Cand *)b)->d;
    return (x > y) - (x < y);
}

/* --------------------------------------------------------------- visited -- */
typedef struct {
    uint32_t *slots;
    size_t cap, mask, n;
} VSet;

static int vset_init(VSet *v, size_t hint) {
    size_t cap = 64;
    while (cap < hint * 2) cap *= 2;
    v->slots = malloc(cap * sizeof(uint32_t));
    if (!v->slots) return -1;
    memset(v->slots, 0xFF, cap * sizeof(uint32_t)); /* NPOS = empty */
    v->cap = cap;
    v->mask = cap - 1;
    v->n = 0;
    return 0;
}
static void vset_free(VSet *v) { free(v->slots); }

static int vset_regrow(VSet *v) {
    size_t ncap = v->cap * 2;
    uint32_t *ns = malloc(ncap * sizeof(uint32_t));
    if (!ns) return -1;
    memset(ns, 0xFF, ncap * sizeof(uint32_t));
    size_t nmask = ncap - 1;
    for (size_t i = 0; i < v->cap; i++) {
        uint32_t e = v->slots[i];
        if (e == NPOS) continue;
        size_t j = mix64(e) & nmask;
        while (ns[j] != NPOS) j = (j + 1) & nmask;
        ns[j] = e;
    }
    free(v->slots);
    v->slots = ns;
    v->cap = ncap;
    v->mask = nmask;
    return 0;
}

/* Returns 1 if newly added, 0 if already present, -1 on OOM. */
static int vset_add(VSet *v, uint32_t node) {
    if ((v->n + 1) * 10 > v->cap * 7 && vset_regrow(v) != 0) return -1;
    size_t i = mix64(node) & v->mask;
    while (v->slots[i] != NPOS) {
        if (v->slots[i] == node) return 0;
        i = (i + 1) & v->mask;
    }
    v->slots[i] = node;
    v->n++;
    return 1;
}

/* ----------------------------------------------------------- graph search -- */
/* Greedy single-best descent at an upper layer; navigates through all nodes
 * (tombstoned included) and returns the locally closest node to q. */
static uint32_t greedy_descend(const Hnsw *h, const float *q, float qn,
                               uint32_t ep, int layer) {
    uint32_t cur = ep;
    float cur_d = dist_q(h, q, qn, cur);
    for (;;) {
        int improved = 0;
        const Node *nd = &h->nodes[cur];
        if (layer > nd->top_layer) break;
        uint32_t cnt = nd->link_cnt[layer];
        const uint32_t *nb = nd->links[layer];
        for (uint32_t i = 0; i < cnt; i++) {
            float d = dist_q(h, q, qn, nb[i]);
            if (d < cur_d) {
                cur_d = d;
                cur = nb[i];
                improved = 1;
            }
        }
        if (!improved) break;
    }
    return cur;
}

/* Beam search at `layer` seeded from `seeds`. Fills `W` (a max-heap, root =
 * farthest) with up to `ef` live candidates; tombstoned nodes are traversed for
 * connectivity but never added to W. Returns 0/-1. */
static int search_layer(const Hnsw *h, const float *q, float qn,
                        const uint32_t *seeds, size_t nseed, size_t ef,
                        int layer, Heap *W) {
    VSet vis;
    if (vset_init(&vis, ef * 8 + 64) != 0) return -1;
    Heap C = {0}; /* candidate min-heap (root = nearest) */
    int rc = 0;
    W->n = 0;

    for (size_t i = 0; i < nseed; i++) {
        uint32_t s = seeds[i];
        if (vset_add(&vis, s) != 1) continue;
        float d = dist_q(h, q, qn, s);
        Cand c = {d, s};
        if (heap_push(&C, c, 0) != 0) { rc = -1; goto done; }
        if (!h->nodes[s].deleted && heap_push(W, c, 1) != 0) { rc = -1; goto done; }
    }

    while (C.n > 0) {
        Cand c = heap_pop(&C, 0);          /* nearest unexpanded */
        if (W->n >= ef && c.d > W->a[0].d) /* nothing closer than the beam's worst */
            break;
        const Node *nd = &h->nodes[c.node];
        if (layer > nd->top_layer) continue;
        uint32_t cnt = nd->link_cnt[layer];
        const uint32_t *nb = nd->links[layer];
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t e = nb[i];
            int added = vset_add(&vis, e);
            if (added < 0) { rc = -1; goto done; }
            if (!added) continue;
            float de = dist_q(h, q, qn, e);
            if (W->n < ef || de < W->a[0].d) {
                Cand ce = {de, e};
                if (heap_push(&C, ce, 0) != 0) { rc = -1; goto done; }
                if (!h->nodes[e].deleted) {
                    if (heap_push(W, ce, 1) != 0) { rc = -1; goto done; }
                    if (W->n > ef) heap_pop(W, 1); /* drop the farthest */
                }
            }
        }
    }
done:
    free(C.a);
    vset_free(&vis);
    return rc;
}

/* ------------------------------------------------------------------ build -- */
static int node_layer_cap(const Hnsw *h, int layer) {
    return layer == 0 ? (int)h->M0 : (int)h->M;
}

/* Cosine distance (1 - similarity) between two stored nodes. */
static float dist_nn(const Hnsw *h, uint32_t a, uint32_t b) {
    const Node *na = &h->nodes[a], *nb = &h->nodes[b];
    double dot = node_dot_node(h, na, nb);
    float den = na->norm * nb->norm;
    return den > 0 ? 1.0f - (float)(dot / den) : 1.0f;
}

/* Diversity neighbour selection (Malkov & Yashunin, Algorithm 4): from the
 * candidates (each .d = distance to the target node), keep up to M that are
 * closer to the target than to any already-kept neighbour. This preserves
 * long-range links so an angularly isolated point stays reachable — the plain
 * "keep the M closest" rule strands such points and tanks recall. Remaining
 * slots are backfilled with the nearest leftovers for connectivity. Writes the
 * chosen node indices to `out` and returns the count; sorts `cand` ascending. */
static size_t select_heuristic(const Hnsw *h, Cand *cand, size_t nc, size_t M,
                               uint32_t *out) {
    qsort(cand, nc, sizeof(Cand), cmp_cand_asc);
    size_t r = 0;
    for (size_t i = 0; i < nc && r < M; i++) {
        int good = 1;
        for (size_t j = 0; j < r; j++)
            if (dist_nn(h, cand[i].node, out[j]) < cand[i].d) { good = 0; break; }
        if (good) out[r++] = cand[i].node;
    }
    for (size_t i = 0; i < nc && r < M; i++) {
        int in = 0;
        for (size_t j = 0; j < r; j++)
            if (out[j] == cand[i].node) { in = 1; break; }
        if (!in) out[r++] = cand[i].node;
    }
    return r;
}

/* Add a directed link a -> b at `layer`; when a's list is full, re-select the
 * capacity-many best neighbours (current ones + b) with the diversity heuristic
 * so connectivity is preserved rather than just keeping the closest. */
static void connect(Hnsw *h, uint32_t a, uint32_t b, int layer) {
    Node *na = &h->nodes[a];
    size_t cap = (size_t)node_layer_cap(h, layer);
    if (na->link_cnt[layer] < cap) {
        na->links[layer][na->link_cnt[layer]++] = b;
        return;
    }
    Cand *cand = h->scratch_cand; /* sized M0 + 1 */
    size_t nc = 0;
    for (uint32_t i = 0; i < na->link_cnt[layer]; i++) {
        cand[nc].node = na->links[layer][i];
        cand[nc].d = dist_nn(h, a, na->links[layer][i]);
        nc++;
    }
    cand[nc].node = b;
    cand[nc].d = dist_nn(h, a, b);
    nc++;
    size_t ns = select_heuristic(h, cand, nc, cap, h->scratch_sel);
    for (size_t i = 0; i < ns; i++) na->links[layer][i] = h->scratch_sel[i];
    na->link_cnt[layer] = (uint32_t)ns;
}

static int rand_level(Hnsw *h) {
    uint64_t r = xorshift(&h->rng);
    double u = ((r >> 11) + 1) * (1.0 / 9007199254740993.0); /* (0,1) */
    int lvl = (int)(-log(u) * h->mL);
    return lvl < 0 ? 0 : lvl;
}

/* Store `vec` into `nd` per the index mode: a float copy, or int8 quantized
 * (symmetric, scale = max|v|/127) with the dequantized L2 norm. 0/-1. */
static int store_vector(const Hnsw *h, Node *nd, const float *vec) {
    if (!h->quantized) {
        float *f = malloc(h->dim * sizeof(float));
        if (!f) return -1;
        memcpy(f, vec, h->dim * sizeof(float));
        nd->vec = f;
        nd->qvec = NULL;
        nd->norm = l2norm(vec, h->dim);
        return 0;
    }
    int8_t *q = malloc(h->dim ? h->dim : 1);
    if (!q) return -1;
    float maxa = 0;
    for (size_t i = 0; i < h->dim; i++) {
        float a = fabsf(vec[i]);
        if (a > maxa) maxa = a;
    }
    float scale = maxa > 0 ? maxa / 127.0f : 1.0f; /* zero vector -> all-zero q */
    double sq = 0;
    for (size_t i = 0; i < h->dim; i++) {
        long r = lroundf(vec[i] / scale);
        if (r > 127) r = 127;
        else if (r < -127) r = -127;
        q[i] = (int8_t)r;
        sq += (double)r * r;
    }
    nd->qvec = q;
    nd->vec = NULL;
    nd->scale = scale;
    nd->norm = (float)(scale * sqrt(sq)); /* norm of the dequantized vector */
    return 0;
}

/* Allocate a fresh node (index returned via *out). Vector is copied/quantized. */
static int node_create(Hnsw *h, uint64_t id, const float *vec, int level,
                       uint32_t *out) {
    if (h->n == h->cap) {
        size_t nc = h->cap ? h->cap * 2 : 64;
        Node *nn = realloc(h->nodes, nc * sizeof(Node));
        if (!nn) return -1;
        h->nodes = nn;
        h->cap = nc;
    }
    Node *nd = &h->nodes[h->n];
    memset(nd, 0, sizeof(*nd));
    nd->id = id;
    if (store_vector(h, nd, vec) != 0) return -1;
    nd->top_layer = level;
    nd->links = calloc((size_t)level + 1, sizeof(uint32_t *));
    nd->link_cnt = calloc((size_t)level + 1, sizeof(uint32_t));
    if (!nd->links || !nd->link_cnt) {
        free(nd->vec);
        free(nd->qvec);
        free(nd->links);
        free(nd->link_cnt);
        return -1;
    }
    for (int l = 0; l <= level; l++) {
        size_t cap = (size_t)node_layer_cap(h, l);
        nd->links[l] = malloc(cap * sizeof(uint32_t));
        if (!nd->links[l]) {
            for (int k = 0; k < l; k++) free(nd->links[k]);
            free(nd->vec);
            free(nd->qvec);
            free(nd->links);
            free(nd->link_cnt);
            return -1;
        }
    }
    *out = (uint32_t)h->n++;
    return 0;
}

/* forward declarations for the tombstone-compaction rebuild */
static Hnsw *hnsw_alloc(size_t dim, size_t M, size_t ef_construction,
                        size_t ef_search, uint64_t rng, int quantize);
static void hnsw_free_contents(Hnsw *h);
#define HNSW_REBUILD_MIN 1024 /* don't compact graphs smaller than this */

/* When tombstoned nodes reach half the array, rebuild the graph from the live
 * nodes so their vectors + link arrays are reclaimed and search stops wading
 * through dead nodes. Amortized O(log n) per delete/replace. Best-effort: on
 * allocation failure the original (un-compacted) graph is kept. */
static int maybe_rebuild(Hnsw *h) {
    if (h->n < HNSW_REBUILD_MIN || h->live == 0 || h->n < 2 * h->live) return 0;
    Hnsw *t = hnsw_alloc(h->dim, h->M, h->ef_construction, h->ef_search, h->rng,
                         h->quantized);
    if (!t) return -1;
    for (size_t i = 0; i < h->n; i++) {
        if (h->nodes[i].deleted) continue;
        if (hnsw_add(t, h->nodes[i].id, h->nodes[i].vec, h->dim) != 0) {
            hnsw_free(t);
            return -1;
        }
    }
    hnsw_free_contents(h);
    *h = *t;  /* adopt the compacted graph's storage */
    free(t);  /* free only the temporary shell */
    return 0;
}

int hnsw_add(Hnsw *h, uint64_t id, const float *vec, size_t dim) {
    if (dim != h->dim) return -1;

    /* replace: tombstone any existing node for this id */
    uint32_t prev = map_get(h, id);
    if (prev != NPOS && !h->nodes[prev].deleted) {
        h->nodes[prev].deleted = 1;
        h->live--;
    }

    int level = rand_level(h);
    uint32_t cur;
    if (node_create(h, id, vec, level, &cur) != 0) return -1;
    if (map_put(h, id, cur) != 0) return -1;
    h->live++;

    /* Use the caller's float vector as the query for the insertion search — the
     * stored node may be int8-quantized (no float copy), and this is exactly
     * how a real query is scored against the graph. */
    const float *q = vec;
    float qn = l2norm(vec, h->dim);

    if (h->entry == NPOS) {
        h->entry = cur;
        h->max_layer = level;
        return 0;
    }

    uint32_t ep = h->entry;
    /* descend from the top to just above the new node's level */
    for (int lc = h->max_layer; lc > level; lc--)
        ep = greedy_descend(h, q, qn, ep, lc);

    Heap W = {0};
    int rc = 0;
    int start = level < h->max_layer ? level : h->max_layer;
    for (int lc = start; lc >= 0; lc--) {
        if (search_layer(h, q, qn, &ep, 1, h->ef_construction, lc, &W) != 0) {
            rc = -1;
            break;
        }
        if (W.n == 0) continue; /* only tombstoned around: leave unlinked here */
        size_t cap = (size_t)node_layer_cap(h, lc);
        /* pick this node's neighbours from W with the diversity heuristic. Copy
         * them out first: connect() reuses h->scratch_sel for its own pruning. */
        uint32_t chosen[64 + 1]; /* M0 <= 64 for any sane M; guarded below */
        size_t want = cap < 64 ? cap : 64;
        size_t ns = select_heuristic(h, W.a, W.n, want, chosen);
        for (size_t i = 0; i < ns; i++) {
            connect(h, cur, chosen[i], lc); /* cur has room (ns <= cap) */
            connect(h, chosen[i], cur, lc); /* may prune chosen[i] */
        }
        ep = W.a[0].node; /* select_heuristic sorted W.a asc -> nearest first */
    }
    free(W.a);
    if (rc != 0) return -1;

    if (level > h->max_layer) {
        h->max_layer = level;
        h->entry = cur;
    }
    (void)maybe_rebuild(h); /* compact if a replace pushed tombstones over half */
    return 0;
}

void hnsw_remove(Hnsw *h, uint64_t id) {
    uint32_t node = map_get(h, id);
    if (node == NPOS || h->nodes[node].deleted) return;
    h->nodes[node].deleted = 1;
    h->live--;
    map_del(h, id);
    (void)maybe_rebuild(h);
}

int hnsw_search(const Hnsw *h, const float *query, size_t dim, size_t top_k,
                size_t ef_search, uint64_t **out_ids, float **out_scores,
                size_t *out_n) {
    if (dim != h->dim) return -1;
    if (ef_search == 0) ef_search = h->ef_search;
    if (ef_search < top_k) ef_search = top_k;

    *out_ids = NULL;
    *out_scores = NULL;
    *out_n = 0;
    if (h->entry == NPOS || top_k == 0) {
        *out_ids = malloc(sizeof(uint64_t));
        *out_scores = malloc(sizeof(float));
        return (*out_ids && *out_scores) ? 0 : -1;
    }

    float qn = l2norm(query, dim);
    uint32_t ep = h->entry;
    for (int lc = h->max_layer; lc > 0; lc--)
        ep = greedy_descend(h, query, qn, ep, lc);

    Heap W = {0};
    if (search_layer(h, query, qn, &ep, 1, ef_search, 0, &W) != 0) {
        free(W.a);
        return -1;
    }

    /* W holds up to ef_search live candidates; sort ascending by distance and
     * keep the top_k nearest (smallest distance == highest similarity). */
    qsort(W.a, W.n, sizeof(Cand), cmp_cand_asc);
    size_t k = W.n < top_k ? W.n : top_k;
    uint64_t *ids = malloc((k ? k : 1) * sizeof(uint64_t));
    float *sc = malloc((k ? k : 1) * sizeof(float));
    if (!ids || !sc) {
        free(ids);
        free(sc);
        free(W.a);
        return -1;
    }
    for (size_t i = 0; i < k; i++) {
        ids[i] = h->nodes[W.a[i].node].id;
        sc[i] = 1.0f - W.a[i].d; /* back to cosine similarity */
    }
    free(W.a);
    *out_ids = ids;
    *out_scores = sc;
    *out_n = k;
    return 0;
}

size_t hnsw_count(const Hnsw *h) { return h ? h->live : 0; }
int hnsw_is_quantized(const Hnsw *h) { return h ? h->quantized : 0; }

int hnsw_foreach_live(const Hnsw *h,
                      int (*cb)(uint64_t id, const float *vec, void *ctx),
                      void *ctx) {
    /* In quant mode there is no float vector; dequantize into a reused temp so
     * the callback still receives a float view. */
    float *tmp = NULL;
    if (h->quantized) {
        tmp = malloc((h->dim ? h->dim : 1) * sizeof(float));
        if (!tmp) return -1;
    }
    int rc = 0;
    for (size_t i = 0; i < h->n; i++) {
        if (h->nodes[i].deleted) continue;
        const float *v;
        if (h->quantized) {
            const int8_t *q = h->nodes[i].qvec;
            float s = h->nodes[i].scale;
            for (size_t d = 0; d < h->dim; d++) tmp[d] = (float)q[d] * s;
            v = tmp;
        } else {
            v = h->nodes[i].vec;
        }
        rc = cb(h->nodes[i].id, v, ctx);
        if (rc) break;
    }
    free(tmp);
    return rc;
}

/* Allocate an empty index with the given (already-resolved) parameters and rng
 * state. Shared by hnsw_create and hnsw_load. */
static Hnsw *hnsw_alloc(size_t dim, size_t M, size_t ef_construction,
                        size_t ef_search, uint64_t rng, int quantize) {
    Hnsw *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->dim = dim;
    h->M = M < 2 ? 2 : M;
    h->M0 = h->M * 2;
    h->ef_construction = ef_construction;
    h->ef_search = ef_search;
    h->quantized = quantize;
    h->rng = rng;
    h->mL = 1.0 / log((double)h->M);
    h->entry = NPOS;
    h->max_layer = 0;
    h->scratch_cand = malloc((h->M0 + 1) * sizeof(Cand));
    h->scratch_sel = malloc(h->M0 * sizeof(uint32_t));
    if (!h->scratch_cand || !h->scratch_sel) {
        free(h->scratch_cand);
        free(h->scratch_sel);
        free(h);
        return NULL;
    }
    return h;
}

Hnsw *hnsw_create(size_t dim, const HnswParams *params) {
    size_t M = (params && params->M) ? params->M : 16;
    size_t efc = (params && params->ef_construction) ? params->ef_construction : 200;
    size_t efs = (params && params->ef_search) ? params->ef_search : 50;
    uint64_t seed = (params && params->seed) ? params->seed : 0x9E3779B97F4A7C15ULL;
    int quantize = params ? params->quantize : 0;
    return hnsw_alloc(dim, M, efc, efs, seed, quantize);
}

static void hnsw_free_contents(Hnsw *h) {
    for (size_t i = 0; i < h->n; i++) {
        Node *nd = &h->nodes[i];
        free(nd->vec);
        free(nd->qvec);
        for (int l = 0; l <= nd->top_layer; l++) free(nd->links[l]);
        free(nd->links);
        free(nd->link_cnt);
    }
    free(h->nodes);
    free(h->map);
    free(h->scratch_cand);
    free(h->scratch_sel);
}

void hnsw_free(Hnsw *h) {
    if (!h) return;
    hnsw_free_contents(h);
    free(h);
}

size_t hnsw_total_nodes(const Hnsw *h) { return h ? h->n : 0; }

/* --------------------------------------------------------- persistence ----
 * A node index is stable across save/load (nodes are written in array order,
 * tombstones included), so the neighbour links stay valid. Scalars are written
 * in native byte order — checkpoints are local files, like the hash index's. */

typedef struct { uint8_t *p; size_t n, cap; int err; } Buf;

static void buf_put(Buf *b, const void *src, size_t len) {
    if (b->err) return;
    if (b->n + len > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->n + len) nc *= 2;
        uint8_t *np = realloc(b->p, nc);
        if (!np) { b->err = 1; return; }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, src, len);
    b->n += len;
}
static void buf_u32(Buf *b, uint32_t v) { buf_put(b, &v, sizeof v); }
static void buf_u64(Buf *b, uint64_t v) { buf_put(b, &v, sizeof v); }

typedef struct { const uint8_t *p; size_t n, off; int err; } Rd;

static void rd_get(Rd *r, void *dst, size_t len) {
    if (r->err || r->off + len > r->n) { r->err = 1; return; }
    memcpy(dst, r->p + r->off, len);
    r->off += len;
}
static uint32_t rd_u32(Rd *r) { uint32_t v = 0; rd_get(r, &v, sizeof v); return v; }
static uint64_t rd_u64(Rd *r) { uint64_t v = 0; rd_get(r, &v, sizeof v); return v; }

int hnsw_save(const Hnsw *h, const char *path, uint64_t covered_log_size) {
    Buf b = {0};
    buf_put(&b, HNSW_FORMAT_MAGIC, 4);
    buf_u32(&b, HNSW_FORMAT_VERSION);
    buf_u32(&b, (uint32_t)h->dim);
    buf_u32(&b, (uint32_t)h->M);
    buf_u32(&b, (uint32_t)h->ef_construction);
    buf_u32(&b, (uint32_t)h->ef_search);
    buf_u32(&b, (uint32_t)(h->quantized ? 1 : 0));
    buf_u64(&b, h->rng); /* live PRNG state: future inserts continue the stream */
    buf_u64(&b, (uint64_t)h->n);
    buf_u64(&b, (uint64_t)h->live);
    buf_u32(&b, h->entry);
    buf_u32(&b, (uint32_t)h->max_layer);
    buf_u64(&b, covered_log_size);
    for (size_t i = 0; i < h->n; i++) {
        const Node *nd = &h->nodes[i];
        buf_u64(&b, nd->id);
        uint32_t flags = (uint32_t)(nd->deleted ? 1 : 0);
        buf_u32(&b, flags);
        buf_u32(&b, (uint32_t)nd->top_layer);
        buf_put(&b, &nd->norm, sizeof(float));
        if (h->quantized) {
            buf_put(&b, &nd->scale, sizeof(float));
            buf_put(&b, nd->qvec, h->dim); /* dim int8 bytes */
        } else {
            buf_put(&b, nd->vec, h->dim * sizeof(float));
        }
        for (int l = 0; l <= nd->top_layer; l++) {
            buf_u32(&b, nd->link_cnt[l]);
            buf_put(&b, nd->links[l], nd->link_cnt[l] * sizeof(uint32_t));
        }
    }
    if (b.err) { free(b.p); return -1; }
    buf_u32(&b, crc32_compute(b.p, b.n)); /* trailing CRC over everything above */
    if (b.err) { free(b.p); return -1; }

    char tmp[1300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(b.p); return -1; }
    int ok = 1;
    for (size_t off = 0; off < b.n;) {
        ssize_t w = write(fd, b.p + off, b.n - off);
        if (w < 0) { if (errno == EINTR) continue; ok = 0; break; }
        off += (size_t)w;
    }
    if (ok) ok = (fsync(fd) == 0);
    close(fd);
    free(b.p);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

Hnsw *hnsw_load(const char *path, size_t expected_dim,
                uint64_t *out_covered_log_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 4 + 4 || lseek(fd, 0, SEEK_SET) != 0) { close(fd); return NULL; }
    uint8_t *buf = malloc((size_t)size);
    if (!buf) { close(fd); return NULL; }
    for (size_t off = 0; off < (size_t)size;) {
        ssize_t r = read(fd, buf + off, (size_t)size - off);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; close(fd); free(buf); return NULL; }
        off += (size_t)r;
    }
    close(fd);

    size_t body = (size_t)size - 4; /* trailing CRC */
    uint32_t want;
    memcpy(&want, buf + body, 4);
    if (crc32_compute(buf, body) != want) { free(buf); return NULL; }

    Rd r = {buf, body, 0, 0};
    char magic[4];
    rd_get(&r, magic, 4);
    if (r.err || memcmp(magic, HNSW_FORMAT_MAGIC, 4) != 0) { free(buf); return NULL; }
    if (rd_u32(&r) != HNSW_FORMAT_VERSION) { free(buf); return NULL; }
    size_t dim = rd_u32(&r);
    if (dim != expected_dim) { free(buf); return NULL; }
    size_t M = rd_u32(&r);
    size_t efc = rd_u32(&r);
    size_t efs = rd_u32(&r);
    int quantized = (int)rd_u32(&r);
    uint64_t rng = rd_u64(&r);
    uint64_t ncount = rd_u64(&r);
    uint64_t live = rd_u64(&r);
    uint32_t entry = rd_u32(&r);
    int max_layer = (int)rd_u32(&r);
    uint64_t covered = rd_u64(&r);
    if (r.err) { free(buf); return NULL; }

    Hnsw *h = hnsw_alloc(dim, M, efc, efs, rng, quantized);
    if (!h) { free(buf); return NULL; }
    if (ncount > 0) {
        h->nodes = calloc((size_t)ncount, sizeof(Node));
        if (!h->nodes) { hnsw_free(h); free(buf); return NULL; }
        h->cap = (size_t)ncount;
    }

    for (uint64_t i = 0; i < ncount; i++) {
        Node *nd = &h->nodes[i];
        nd->id = rd_u64(&r);
        nd->deleted = (int)rd_u32(&r);
        nd->top_layer = (int)rd_u32(&r);
        if (r.err || nd->top_layer < 0) goto bad;
        nd->links = calloc((size_t)nd->top_layer + 1, sizeof(uint32_t *));
        nd->link_cnt = calloc((size_t)nd->top_layer + 1, sizeof(uint32_t));
        if (!nd->links || !nd->link_cnt) goto bad;
        if (h->quantized) {
            nd->qvec = malloc(dim ? dim : 1);
            if (!nd->qvec) goto bad;
        } else {
            nd->vec = malloc(dim * sizeof(float));
            if (!nd->vec) goto bad;
        }
        h->n = i + 1; /* keep the freeable prefix accurate for the error path */
        rd_get(&r, &nd->norm, sizeof(float));
        if (h->quantized) {
            rd_get(&r, &nd->scale, sizeof(float));
            rd_get(&r, nd->qvec, dim); /* dim int8 bytes */
        } else {
            rd_get(&r, nd->vec, dim * sizeof(float));
        }
        for (int l = 0; l <= nd->top_layer; l++) {
            uint32_t cnt = rd_u32(&r);
            size_t cap = (size_t)node_layer_cap(h, l);
            if (r.err || cnt > cap) goto bad;
            nd->links[l] = malloc(cap * sizeof(uint32_t));
            if (!nd->links[l]) goto bad;
            nd->link_cnt[l] = cnt;
            rd_get(&r, nd->links[l], cnt * sizeof(uint32_t));
        }
    }
    if (r.err || r.off != body) goto bad; /* trailing garbage or short read */

    h->live = live;
    h->entry = entry;
    h->max_layer = max_layer;
    /* Pre-size the id->node map for the whole live set before rebuilding it:
     * map_put grows against h->live, so a tiny initial map would otherwise
     * overflow (and spin) under a bulk load. Keep the load factor < 0.7. */
    size_t mcap = 64;
    while (mcap * 7 < (live + 1) * 10) mcap *= 2;
    if (map_grow(h, mcap) != 0) goto bad;
    for (size_t i = 0; i < h->n; i++)
        if (!h->nodes[i].deleted && map_put(h, h->nodes[i].id, (uint32_t)i) != 0)
            goto bad;
    free(buf);
    if (out_covered_log_size) *out_covered_log_size = covered;
    return h;
bad:
    hnsw_free(h);
    free(buf);
    return NULL;
}