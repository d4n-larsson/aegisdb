/* Semantic index (T035): exact cosine top-K over stored float32 vectors. */
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

struct SemanticIndex {
    size_t dim;
    SemEntry *e;
    size_t n;
    size_t cap;
};

SemanticIndex *semantic_index_create(size_t dim) {
    SemanticIndex *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->dim = dim;
    return s;
}

void semantic_index_free(SemanticIndex *s) {
    if (!s) return;
    for (size_t i = 0; i < s->n; i++) free(s->e[i].vec);
    free(s->e);
    free(s);
}

static SemEntry *find(SemanticIndex *s, uint64_t id) {
    for (size_t i = 0; i < s->n; i++)
        if (s->e[i].used && s->e[i].id == id) return &s->e[i];
    return NULL;
}

static float l2norm(const float *v, size_t dim) {
    double acc = 0;
    for (size_t i = 0; i < dim; i++) acc += (double)v[i] * v[i];
    return (float)sqrt(acc);
}

int semantic_index_add(SemanticIndex *s, uint64_t id, const float *vec,
                       size_t dim) {
    if (dim != s->dim) return -1;
    SemEntry *e = find(s, id);
    if (!e) {
        if (s->n == s->cap) {
            size_t cap = s->cap ? s->cap * 2 : 64;
            SemEntry *ne = realloc(s->e, cap * sizeof(SemEntry));
            if (!ne) return -1;
            s->e = ne;
            s->cap = cap;
        }
        e = &s->e[s->n++];
        e->vec = NULL;
    }
    float *nv = realloc(e->vec, dim * sizeof(float));
    if (!nv) return -1;
    memcpy(nv, vec, dim * sizeof(float));
    e->vec = nv;
    e->id = id;
    e->norm = l2norm(vec, dim);
    e->used = 1;
    return 0;
}

void semantic_index_remove(SemanticIndex *s, uint64_t id) {
    SemEntry *e = find(s, id);
    if (e) e->used = 0;
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