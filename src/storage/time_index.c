/* Temporal range index (T027): sorted (created,id) array. */
#include "aegisdb/time_index.h"

#include <stdlib.h>
#include <string.h>

TimeIndex *time_index_create(void) {
    TimeIndex *t = calloc(1, sizeof(*t));
    return t;
}

void time_index_free(TimeIndex *t) {
    if (!t) return;
    free(t->e);
    free(t);
}

static int cmp_key(uint64_t ca, uint64_t ia, uint64_t cb, uint64_t ib) {
    if (ca < cb) return -1;
    if (ca > cb) return 1;
    if (ia < ib) return -1;
    if (ia > ib) return 1;
    return 0;
}

/* First index with key >= (created,id). */
static size_t lower_bound(const TimeIndex *t, uint64_t created, uint64_t id) {
    size_t lo = 0, hi = t->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cmp_key(t->e[mid].created, t->e[mid].id, created, id) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int time_index_add(TimeIndex *t, uint64_t created, uint64_t id) {
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 256;
        TimeEntry *ne = realloc(t->e, cap * sizeof(TimeEntry));
        if (!ne) return -1;
        t->e = ne;
        t->cap = cap;
    }
    size_t pos = lower_bound(t, created, id);
    if (pos < t->n)
        memmove(&t->e[pos + 1], &t->e[pos], (t->n - pos) * sizeof(TimeEntry));
    t->e[pos].created = created;
    t->e[pos].id = id;
    t->n++;
    return 0;
}

void time_index_remove(TimeIndex *t, uint64_t created, uint64_t id) {
    size_t pos = lower_bound(t, created, id);
    if (pos < t->n && t->e[pos].created == created && t->e[pos].id == id) {
        memmove(&t->e[pos], &t->e[pos + 1],
                (t->n - pos - 1) * sizeof(TimeEntry));
        t->n--;
    }
}

int time_index_range(const TimeIndex *t, uint64_t start, uint64_t end,
                     size_t max, uint64_t **out_ids, size_t *out_n) {
    size_t pos = lower_bound(t, start, 0);
    size_t cap = 16, n = 0;
    uint64_t *ids = malloc(cap * sizeof(uint64_t));
    if (!ids) return -1;
    for (size_t i = pos; i < t->n; i++) {
        if (t->e[i].created > end) break;
        if (max && n >= max) break;
        if (n == cap) {
            cap *= 2;
            uint64_t *ni = realloc(ids, cap * sizeof(uint64_t));
            if (!ni) {
                free(ids);
                return -1;
            }
            ids = ni;
        }
        ids[n++] = t->e[i].id;
    }
    *out_ids = ids;
    *out_n = n;
    return 0;
}