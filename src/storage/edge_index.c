/* Reverse relationship index (ROADMAP 5.1): to_id -> sources pointing at it. */
#include "aegisdb/edge_index.h"

#include <stdlib.h>
#include <string.h>

#include "aegisdb/hash_mix.h"

/* Kind interning: string-keyed and low-cardinality, so a fixed chained table in
 * the shape of tag_index. The target table below is id-keyed and scales with the
 * corpus, so it grows instead (the lexical index's doc-table shape). */
#define EDGE_KIND_NBUCKETS 1024

/* Reserved kind ids. Interned kinds run 1..EDGE_MAX_KINDS, comfortably clear of
 * the sentinels, so a kind id always fits a uint16_t. */
#define EDGE_KIND_NONE 0        /* edge stored without a kind */
#define EDGE_KIND_UNKNOWN 65535 /* not internable: cap hit, or over-long */

typedef struct KindNode {
    char *name;
    uint16_t id;
    struct KindNode *next;
} KindNode;

/* One incoming edge. 16 bytes with natural alignment; packing it to 10 would
 * save a third of the postings at the cost of split parallel arrays (two
 * memmoves and two failure paths per insert), which is not worth it. */
typedef struct {
    uint64_t from_id;
    uint16_t kind;
} EdgePost;

#define EDGE_SLOT_EMPTY 0
#define EDGE_SLOT_LIVE 1
#define EDGE_SLOT_DEAD 2

/* Postings for one target. Open-addressed by to_id, linear probing. */
typedef struct {
    uint64_t to_id;
    EdgePost *posts; /* sorted by (from_id, kind) ascending */
    size_t n;
    size_t cap;
    uint8_t state;
} EdgeTarget;

struct EdgeIndex {
    EdgeTarget *targets;
    size_t tgt_cap;  /* power of two (0 until the first insert) */
    size_t tgt_live; /* targets with at least one edge */
    size_t tgt_used; /* live + dead slots, for the load factor */
    size_t edges;    /* total postings */

    KindNode *kind_buckets[EDGE_KIND_NBUCKETS];
    char **kind_by_id; /* index i holds the name of kind id i+1 */
    size_t kind_count;
    size_t kind_by_id_cap;
};

/* --- kind interning ------------------------------------------------------- */

static size_t kind_hash(const char *s) {
    size_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h % EDGE_KIND_NBUCKETS;
}

/* Interned id for `kind`, minting one if needed. Returns EDGE_KIND_NONE for
 * NULL, and EDGE_KIND_UNKNOWN when the kind cannot be interned (over-long, cap
 * reached, or out of memory) — the caller still indexes the edge. */
static uint16_t kind_intern(EdgeIndex *e, const char *kind) {
    if (!kind) {
        return EDGE_KIND_NONE;
    }
    size_t len = strlen(kind);
    if (len == 0) {
        return EDGE_KIND_NONE; /* an empty kind carries no more than none */
    }
    if (len > EDGE_MAX_KIND_LEN) {
        return EDGE_KIND_UNKNOWN;
    }
    size_t b = kind_hash(kind);
    for (KindNode *n = e->kind_buckets[b]; n; n = n->next) {
        if (strcmp(n->name, kind) == 0) {
            return n->id;
        }
    }
    if (e->kind_count >= EDGE_MAX_KINDS) {
        return EDGE_KIND_UNKNOWN;
    }
    if (e->kind_count == e->kind_by_id_cap) {
        size_t nc = e->kind_by_id_cap ? e->kind_by_id_cap * 2 : 16;
        char **nb = realloc(e->kind_by_id, nc * sizeof(*nb));
        if (!nb) {
            return EDGE_KIND_UNKNOWN;
        }
        e->kind_by_id = nb;
        e->kind_by_id_cap = nc;
    }
    KindNode *n = calloc(1, sizeof(*n));
    if (!n) {
        return EDGE_KIND_UNKNOWN;
    }
    n->name = malloc(len + 1);
    if (!n->name) {
        free(n);
        return EDGE_KIND_UNKNOWN;
    }
    memcpy(n->name, kind, len + 1);
    n->id = (uint16_t)(e->kind_count + 1);
    n->next = e->kind_buckets[b];
    e->kind_buckets[b] = n;
    e->kind_by_id[e->kind_count++] = n->name; /* borrowed; the node owns it */
    return n->id;
}

/* Interned id for `kind` without minting one. EDGE_KIND_UNKNOWN when the kind
 * has never been seen — which for a *query* means "matches nothing", distinct
 * from a posting whose kind is unknown. Callers keep the two apart. */
static uint16_t kind_lookup(const EdgeIndex *e, const char *kind) {
    if (!kind || !*kind) {
        return EDGE_KIND_NONE;
    }
    for (KindNode *n = e->kind_buckets[kind_hash(kind)]; n; n = n->next) {
        if (strcmp(n->name, kind) == 0) {
            return n->id;
        }
    }
    return EDGE_KIND_UNKNOWN;
}

static const char *kind_name(const EdgeIndex *e, uint16_t id) {
    if (id == EDGE_KIND_NONE || id == EDGE_KIND_UNKNOWN) {
        return NULL;
    }
    if ((size_t)id > e->kind_count) {
        return NULL;
    }
    return e->kind_by_id[id - 1];
}

/* --- target table -------------------------------------------------------- */

EdgeIndex *edge_index_create(void) { return calloc(1, sizeof(EdgeIndex)); }

void edge_index_free(EdgeIndex *e) {
    if (!e) {
        return;
    }
    for (size_t i = 0; i < e->tgt_cap; i++) {
        free(e->targets[i].posts);
    }
    free(e->targets);
    for (size_t i = 0; i < EDGE_KIND_NBUCKETS; i++) {
        KindNode *n = e->kind_buckets[i];
        while (n) {
            KindNode *nx = n->next;
            free(n->name);
            free(n);
            n = nx;
        }
    }
    free(e->kind_by_id);
    free(e);
}

static EdgeTarget *tgt_find(const EdgeIndex *e, uint64_t to_id) {
    if (!e->tgt_cap) {
        return NULL;
    }
    size_t mask = e->tgt_cap - 1;
    size_t i = (size_t)mix64(to_id) & mask;
    for (size_t probe = 0; probe < e->tgt_cap; probe++) {
        EdgeTarget *t = &e->targets[(i + probe) & mask];
        if (t->state == EDGE_SLOT_EMPTY) {
            return NULL; /* a never-used slot ends the probe chain */
        }
        if (t->state == EDGE_SLOT_LIVE && t->to_id == to_id) {
            return t;
        }
    }
    return NULL;
}

static int tgt_grow(EdgeIndex *e) {
    size_t ncap = e->tgt_cap ? e->tgt_cap * 2 : 64;
    EdgeTarget *nt = calloc(ncap, sizeof(*nt));
    if (!nt) {
        return -1;
    }
    size_t mask = ncap - 1;
    for (size_t i = 0; i < e->tgt_cap; i++) {
        EdgeTarget *old = &e->targets[i];
        if (old->state != EDGE_SLOT_LIVE) {
            continue; /* the rehash is also what drops the tombstones */
        }
        size_t j = (size_t)mix64(old->to_id) & mask;
        while (nt[j].state == EDGE_SLOT_LIVE) {
            j = (j + 1) & mask;
        }
        nt[j] = *old;
    }
    free(e->targets);
    e->targets = nt;
    e->tgt_cap = ncap;
    e->tgt_used = e->tgt_live;
    return 0;
}

/* Slot for to_id, creating it if absent. NULL on allocation failure. */
static EdgeTarget *tgt_put(EdgeIndex *e, uint64_t to_id) {
    if (!e->tgt_cap || ((e->tgt_used + 1) * 4) >= (e->tgt_cap * 3)) {
        if (tgt_grow(e) != 0) {
            return NULL;
        }
    }
    size_t mask = e->tgt_cap - 1;
    size_t i = (size_t)mix64(to_id) & mask;
    EdgeTarget *reuse = NULL;
    for (size_t probe = 0; probe < e->tgt_cap; probe++) {
        EdgeTarget *t = &e->targets[(i + probe) & mask];
        if (t->state == EDGE_SLOT_LIVE) {
            if (t->to_id == to_id) {
                return t;
            }
            continue;
        }
        if (t->state == EDGE_SLOT_DEAD) {
            /* Remember the first tombstone but keep probing: an existing live
             * slot for this id may lie further along the chain, and claiming the
             * tombstone now would duplicate the target. */
            if (!reuse) {
                reuse = t;
            }
            continue;
        }
        EdgeTarget *slot = reuse ? reuse : t;
        if (slot->state == EDGE_SLOT_EMPTY) {
            e->tgt_used++;
        }
        slot->to_id = to_id;
        slot->posts = NULL;
        slot->n = 0;
        slot->cap = 0;
        slot->state = EDGE_SLOT_LIVE;
        e->tgt_live++;
        return slot;
    }
    return NULL; /* table full: impossible at a 3/4 load factor */
}

/* Retire a target whose last edge just went away, so repeated add/remove of
 * distinct targets cannot grow the probe chains without bound. */
static void tgt_retire(EdgeIndex *e, EdgeTarget *t) {
    free(t->posts);
    t->posts = NULL;
    t->n = 0;
    t->cap = 0;
    t->state = EDGE_SLOT_DEAD;
    e->tgt_live--;
}

/* --- postings ------------------------------------------------------------ */

/* Total order over postings: from_id, then interned kind id. Any consistent
 * order would do — this one exists so an edge is located by binary search rather
 * than a scan. The kind component is the *interned id*, so it is not
 * lexicographic; the header tells callers not to depend on it. */
static int post_cmp(uint64_t a_from, uint16_t a_kind, uint64_t b_from,
                    uint16_t b_kind) {
    if (a_from != b_from) {
        return a_from < b_from ? -1 : 1;
    }
    if (a_kind != b_kind) {
        return a_kind < b_kind ? -1 : 1;
    }
    return 0;
}

/* Index of the first posting >= (from_id, kind). */
static size_t post_lower_bound(const EdgePost *p, size_t n, uint64_t from_id,
                               uint16_t kind) {
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) / 2);
        if (post_cmp(p[mid].from_id, p[mid].kind, from_id, kind) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

int edge_index_add(EdgeIndex *e, uint64_t from_id, uint64_t to_id,
                   const char *kind) {
    if (!e) {
        return 0;
    }
    /* Interning first: it can fail (falling back to UNKNOWN) but must not leave
     * a half-built target behind if it does. */
    uint16_t k = kind_intern(e, kind);
    EdgeTarget *t = tgt_put(e, to_id);
    if (!t) {
        return -1;
    }
    size_t pos = post_lower_bound(t->posts, t->n, from_id, k);
    if (pos < t->n && t->posts[pos].from_id == from_id &&
        t->posts[pos].kind == k) {
        return 0; /* already present: idempotent, as `relate` is */
    }
    if (t->n == t->cap) {
        /* Start at one, not a comfortable 4: the common target in a provenance
         * graph has a single incoming edge (a `supersedes` chain is 1-in), and a
         * 4-slot floor spent 48 bytes of the 64 on nothing. Doubling from 1
         * reaches any real fan-in in a handful of reallocs. */
        size_t nc = t->cap ? t->cap * 2 : 1;
        EdgePost *np = realloc(t->posts, nc * sizeof(*np));
        if (!np) {
            if (t->n == 0) {
                tgt_retire(e,
                           t); /* nothing landed; don't leave an empty slot */
            }
            return -1;
        }
        t->posts = np;
        t->cap = nc;
    }
    if (pos < t->n) {
        memmove(&t->posts[pos + 1], &t->posts[pos],
                (t->n - pos) * sizeof(*t->posts));
    }
    t->posts[pos].from_id = from_id;
    t->posts[pos].kind = k;
    t->n++;
    e->edges++;
    return 0;
}

void edge_index_remove(EdgeIndex *e, uint64_t from_id, uint64_t to_id,
                       const char *kind) {
    if (!e) {
        return;
    }
    EdgeTarget *t = tgt_find(e, to_id);
    if (!t) {
        return;
    }
    /* An un-internable kind was stored as UNKNOWN, and kind_lookup reports a
     * never-seen kind the same way, so both land on the same posting — which is
     * the behaviour we want: remove what add would have written. */
    uint16_t k = kind_lookup(e, kind);
    size_t pos = post_lower_bound(t->posts, t->n, from_id, k);
    if (!(pos < t->n && t->posts[pos].from_id == from_id &&
          t->posts[pos].kind == k)) {
        return;
    }
    memmove(&t->posts[pos], &t->posts[pos + 1],
            (t->n - pos - 1) * sizeof(*t->posts));
    t->n--;
    e->edges--;
    if (t->n == 0) {
        tgt_retire(e, t);
    }
}

void edge_index_remove_target(EdgeIndex *e, uint64_t id) {
    if (!e) {
        return;
    }
    EdgeTarget *t = tgt_find(e, id);
    if (!t) {
        return;
    }
    e->edges -= t->n;
    tgt_retire(e, t);
}

int edge_index_sources(const EdgeIndex *e, uint64_t to_id,
                       const char *const *kinds, size_t n_kinds,
                       EdgeSource **out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    if (!e) {
        return 0;
    }
    EdgeTarget *t = tgt_find(e, to_id);
    if (!t || t->n == 0) {
        return 0;
    }

    /* Resolve the filter to ids once, rather than per posting. A kind the index
     * has never interned matches nothing, so it is dropped here instead of
     * being confused with a posting whose kind is genuinely unknown. */
    uint16_t *want = NULL;
    size_t want_n = 0;
    if (n_kinds) {
        want = malloc(n_kinds * sizeof(*want));
        if (!want) {
            return -1;
        }
        for (size_t i = 0; i < n_kinds; i++) {
            if (!kinds[i]) {
                continue;
            }
            uint16_t k = kind_lookup(e, kinds[i]);
            if (k == EDGE_KIND_UNKNOWN) {
                continue;
            }
            want[want_n++] = k;
        }
    }

    EdgeSource *res = malloc(t->n * sizeof(*res));
    if (!res) {
        free(want);
        return -1;
    }
    size_t cnt = 0;
    for (size_t i = 0; i < t->n; i++) {
        uint16_t k = t->posts[i].kind;
        if (n_kinds) {
            /* A posting whose kind could not be interned is a candidate for
             * every filter: the index does not know what it is, so excluding it
             * would drop a real answer. The caller confirms against the record. */
            int keep = (k == EDGE_KIND_UNKNOWN);
            for (size_t j = 0; !keep && j < want_n; j++) {
                keep = (want[j] == k);
            }
            if (!keep) {
                continue;
            }
        }
        res[cnt].from_id = t->posts[i].from_id;
        res[cnt].kind = kind_name(e, k);
        res[cnt].kind_unknown = (k == EDGE_KIND_UNKNOWN);
        cnt++;
    }
    free(want);
    if (cnt == 0) {
        free(res);
        return 0; /* *out stays NULL: nothing matched */
    }
    *out = res;
    *out_n = cnt;
    return 0;
}

size_t edge_index_edges(const EdgeIndex *e) { return e ? e->edges : 0; }

size_t edge_index_kinds(const EdgeIndex *e) { return e ? e->kind_count : 0; }

size_t edge_index_bytes(const EdgeIndex *e) {
    if (!e) {
        return 0;
    }
    size_t total = sizeof(*e);
    total += e->tgt_cap * sizeof(EdgeTarget);
    for (size_t i = 0; i < e->tgt_cap; i++) {
        total += e->targets[i].cap * sizeof(EdgePost);
    }
    total += e->kind_by_id_cap * sizeof(char *);
    for (size_t i = 0; i < EDGE_KIND_NBUCKETS; i++) {
        for (const KindNode *n = e->kind_buckets[i]; n; n = n->next) {
            total += sizeof(*n) + strlen(n->name) + 1;
        }
    }
    return total;
}
