/* Semantic index (T035): exact cosine top-K over stored float32 vectors.
 *
 * Vectors live in a dense array `e[0..n)`; an open-addressing `id -> slot` map
 * keeps add / lookup / remove O(1) so building or recovering an index of N
 * vectors is O(N), not O(N^2). The map stores the dense index, so a swap-remove
 * only has to repoint the one moved entry. */
#include "aegisdb/semantic_index.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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

SemanticIndex *semantic_index_create(size_t dim) {
    SemanticIndex *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->dim = dim;
    return s;
}

size_t semantic_index_count(const SemanticIndex *s) { return s ? s->n : 0; }

void semantic_index_free(SemanticIndex *s) {
    if (!s) return;
    for (size_t i = 0; i < s->n; i++) free(s->e[i].vec);
    free(s->e);
    free(s->map);
    free(s);
}

static float l2norm(const float *v, size_t dim) {
    double acc = 0;
    for (size_t i = 0; i < dim; i++) acc += (double)v[i] * v[i];
    return (float)sqrt(acc);
}

int semantic_index_add(SemanticIndex *s, uint64_t id, const float *vec,
                       size_t dim) {
    if (dim != s->dim) return -1;
    if (s->mcap == 0 && map_grow(s, MAP_INITIAL_CAP) != 0) return -1;

    size_t j = map_probe(s->map, s->mcap, id);
    if (s->map[j].used) {
        /* replace the vector for an existing id in place */
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
    return 0;
}

void semantic_index_remove(SemanticIndex *s, uint64_t id) {
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

int semantic_index_search(const SemanticIndex *s, const float *query,
                          size_t dim, size_t top_k, uint64_t **out_ids,
                          float **out_scores, size_t *out_n) {
    if (dim != s->dim) return -1;
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
    qsort(all, m, sizeof(Scored), cmp_scored_desc);
    size_t k = (top_k && top_k < m) ? top_k : m;
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