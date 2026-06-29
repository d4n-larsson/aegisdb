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

#include <math.h>
#include <stdlib.h>
#include <string.h>

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
    float *vec;
    float norm;       /* precomputed L2 norm */
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

struct Hnsw {
    size_t dim, M, M0, ef_construction, ef_search;
    double mL;       /* level-generation normalization = 1/ln(M) */
    uint64_t rng;

    Node *nodes;
    size_t n, cap;   /* total nodes incl. tombstoned */
    size_t live;     /* non-tombstoned */
    uint32_t entry;  /* entry-point node, or NPOS when empty */
    int max_layer;

    MapSlot *map;    /* id -> node index; power-of-two capacity */
    size_t mcap;
};

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

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
static float l2norm(const float *v, size_t dim) {
    double acc = 0;
    for (size_t i = 0; i < dim; i++) acc += (double)v[i] * v[i];
    return (float)sqrt(acc);
}

/* 1 - cosine similarity, in [0, 2]; smaller is nearer. */
static float dist_q(const Hnsw *h, const float *q, float qnorm, uint32_t node) {
    const Node *nd = &h->nodes[node];
    double dot = 0;
    const float *v = nd->vec;
    for (size_t i = 0; i < h->dim; i++) dot += (double)q[i] * v[i];
    float denom = qnorm * nd->norm;
    return denom > 0 ? 1.0f - (float)(dot / denom) : 1.0f;
}

/* ------------------------------------------------------------ candidate heaps */
typedef struct {
    float d;
    uint32_t node;
} Cand;

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

/* Add a directed link a -> b at `layer`, keeping a's list to its capacity by
 * evicting a's current farthest neighbour when full. */
static void connect(Hnsw *h, uint32_t a, uint32_t b, int layer) {
    Node *na = &h->nodes[a];
    size_t cap = (size_t)node_layer_cap(h, layer);
    uint32_t *L = na->links[layer];
    if (na->link_cnt[layer] < cap) {
        L[na->link_cnt[layer]++] = b;
        return;
    }
    /* full: replace the neighbour farthest from `a` if `b` is closer */
    float a_norm = na->norm;
    const float *av = na->vec;
    /* distance helper from node a to node x */
    size_t worst = 0;
    float worst_d = -1.0f;
    for (size_t i = 0; i < cap; i++) {
        const Node *nx = &h->nodes[L[i]];
        double dot = 0;
        for (size_t k = 0; k < h->dim; k++) dot += (double)av[k] * nx->vec[k];
        float den = a_norm * nx->norm;
        float d = den > 0 ? 1.0f - (float)(dot / den) : 1.0f;
        if (d > worst_d) { worst_d = d; worst = i; }
    }
    const Node *nb = &h->nodes[b];
    double dotb = 0;
    for (size_t k = 0; k < h->dim; k++) dotb += (double)av[k] * nb->vec[k];
    float denb = a_norm * nb->norm;
    float db = denb > 0 ? 1.0f - (float)(dotb / denb) : 1.0f;
    if (db < worst_d) L[worst] = b;
}

static int rand_level(Hnsw *h) {
    uint64_t r = xorshift(&h->rng);
    double u = ((r >> 11) + 1) * (1.0 / 9007199254740993.0); /* (0,1) */
    int lvl = (int)(-log(u) * h->mL);
    return lvl < 0 ? 0 : lvl;
}

/* Allocate a fresh node (index returned via *out). Vector is copied. */
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
    nd->vec = malloc(h->dim * sizeof(float));
    if (!nd->vec) return -1;
    memcpy(nd->vec, vec, h->dim * sizeof(float));
    nd->norm = l2norm(vec, h->dim);
    nd->top_layer = level;
    nd->links = calloc((size_t)level + 1, sizeof(uint32_t *));
    nd->link_cnt = calloc((size_t)level + 1, sizeof(uint32_t));
    if (!nd->links || !nd->link_cnt) {
        free(nd->vec);
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
            free(nd->links);
            free(nd->link_cnt);
            return -1;
        }
    }
    *out = (uint32_t)h->n++;
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

    float qn = h->nodes[cur].norm;
    const float *q = h->nodes[cur].vec;

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
        /* select the M closest of W as neighbours (W is a max-heap; sort asc) */
        size_t cap = (size_t)node_layer_cap(h, lc);
        /* simple selection: repeatedly pop the max until <= cap remain, leaving
         * the cap nearest in W.a[0..W.n) */
        while (W.n > cap) heap_pop(&W, 1);
        for (size_t i = 0; i < W.n; i++) {
            uint32_t nb = W.a[i].node;
            connect(h, cur, nb, lc);
            connect(h, nb, cur, lc);
        }
        ep = W.a[0].node; /* nearest seen, for the next layer down */
    }
    free(W.a);
    if (rc != 0) return -1;

    if (level > h->max_layer) {
        h->max_layer = level;
        h->entry = cur;
    }
    return 0;
}

void hnsw_remove(Hnsw *h, uint64_t id) {
    uint32_t node = map_get(h, id);
    if (node == NPOS || h->nodes[node].deleted) return;
    h->nodes[node].deleted = 1;
    h->live--;
    map_del(h, id);
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

Hnsw *hnsw_create(size_t dim, const HnswParams *params) {
    Hnsw *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->dim = dim;
    h->M = (params && params->M) ? params->M : 16;
    h->M0 = h->M * 2;
    h->ef_construction =
        (params && params->ef_construction) ? params->ef_construction : 200;
    h->ef_search = (params && params->ef_search) ? params->ef_search : 50;
    h->rng = (params && params->seed) ? params->seed : 0x9E3779B97F4A7C15ULL;
    h->mL = 1.0 / log((double)(h->M < 2 ? 2 : h->M));
    h->entry = NPOS;
    h->max_layer = 0;
    return h;
}

void hnsw_free(Hnsw *h) {
    if (!h) return;
    for (size_t i = 0; i < h->n; i++) {
        Node *nd = &h->nodes[i];
        free(nd->vec);
        for (int l = 0; l <= nd->top_layer; l++) free(nd->links[l]);
        free(nd->links);
        free(nd->link_cnt);
    }
    free(h->nodes);
    free(h->map);
    free(h);
}