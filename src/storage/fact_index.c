/* Fact indexes (ROADMAP 5.2): subject, object, and predicate -> records. */
#include "aegisdb/fact_index.h"

#include <stdlib.h>
#include <string.h>

#include "aegisdb/hash_mix.h"
#include "aegisdb/logging.h"

/* Predicate interning: string-keyed and low-cardinality, so a fixed chained
 * table (tag_index's shape). The subject/object tables are keyed by values that
 * scale with the corpus, so they grow instead (the lexical index's doc-table
 * shape). */
#define PRED_NBUCKETS 1024

/* Reserved: interned predicate ids run 1..FACT_MAX_PREDICATES. 0 means "no
 * predicate", used only as the wildcard sentinel in a lookup. */
#define PRED_NONE 0

typedef struct PredNode {
    char *name;
    uint16_t id;
    size_t uses; /* postings carrying this predicate */
    struct PredNode *next;
} PredNode;

/* One (predicate, record) posting under a subject or an object. */
typedef struct {
    uint64_t record_id;
    uint16_t pred;
} Post;

#define SLOT_EMPTY 0
#define SLOT_LIVE 1
#define SLOT_DEAD 2

/* A subject, or an object, with the postings filed under it. `obj_str` is owned
 * and set only for a string-valued object; `key` holds the subject id, the
 * object id, or a string object's hash. */
typedef struct {
    uint64_t key;
    char *obj_str; /* NULL unless this is a string-valued object slot */
    Post *posts;   /* sorted by (pred, record_id) ascending */
    size_t n;
    size_t cap;
    uint8_t state;
} Slot;

typedef struct {
    Slot *slots;
    size_t cap;  /* power of two; 0 until the first insert */
    size_t live; /* slots holding at least one posting */
    size_t used; /* live + dead, for the load factor */
} Table;

struct FactIndex {
    Table subj; /* subject id           -> [(pred, record)] */
    Table obj;  /* object id or hash    -> [(pred, record)] */
    /* predicate -> records, indexed by (pred id - 1). Predicates are capped and
     * few, so a flat array beats a hash table and needs no probing at all. */
    uint64_t *pred_recs[FACT_MAX_PREDICATES];
    size_t pred_n[FACT_MAX_PREDICATES];
    size_t pred_cap[FACT_MAX_PREDICATES];

    PredNode *pred_buckets[PRED_NBUCKETS];
    PredNode **pred_by_id; /* index i is the node for predicate id i+1 */
    size_t pred_count;     /* predicates ever interned; never shrinks */
    size_t preds_live;     /* predicates with at least one posting */
    size_t pred_by_id_cap;

    size_t facts; /* total indexed facts */
};

/* ----- predicate interning ----------------------------------------------- */

static size_t pred_hash(const char *s) {
    size_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h % PRED_NBUCKETS;
}

static PredNode *pred_find(const FactIndex *f, const char *p) {
    for (PredNode *n = f->pred_buckets[pred_hash(p)]; n; n = n->next) {
        if (strcmp(n->name, p) == 0) {
            return n;
        }
    }
    return NULL;
}

/* Interned id for `p`, minting one if needed. 0 when it cannot be interned —
 * over-long, cap reached, or out of memory. Unlike edge_index's kinds, an
 * un-internable predicate is *refused* rather than recorded as unknown: a fact
 * index that cannot name a predicate cannot answer a pattern about it, and
 * silently keeping an unqueryable entry would be worse than declining the fact
 * and telling the caller. */
static uint16_t pred_intern(FactIndex *f, const char *p) {
    size_t len = strlen(p);
    if (len == 0 || len > FACT_MAX_PREDICATE_LEN) {
        return PRED_NONE;
    }
    PredNode *n = pred_find(f, p);
    if (n) {
        return n->id;
    }
    if (f->pred_count >= FACT_MAX_PREDICATES) {
        return PRED_NONE;
    }
    if (f->pred_count == f->pred_by_id_cap) {
        size_t nc = f->pred_by_id_cap ? f->pred_by_id_cap * 2 : 16;
        PredNode **nb = realloc(f->pred_by_id, nc * sizeof(*nb));
        if (!nb) {
            return PRED_NONE;
        }
        f->pred_by_id = nb;
        f->pred_by_id_cap = nc;
    }
    n = calloc(1, sizeof(*n));
    if (!n) {
        return PRED_NONE;
    }
    n->name = malloc(len + 1);
    if (!n->name) {
        free(n);
        return PRED_NONE;
    }
    memcpy(n->name, p, len + 1);
    n->id = (uint16_t)(f->pred_count + 1);
    size_t b = pred_hash(p);
    n->next = f->pred_buckets[b];
    f->pred_buckets[b] = n;
    f->pred_by_id[f->pred_count++] = n;
    return n->id;
}

/* Interned id without minting. 0 when never seen, which for a *lookup* means
 * "matches nothing". */
static uint16_t pred_lookup(const FactIndex *f, const char *p) {
    if (!p || !*p) {
        return PRED_NONE;
    }
    PredNode *n = pred_find(f, p);
    return n ? n->id : PRED_NONE;
}

/* A posting carrying `id` was added or dropped. The intern table never shrinks
 * (ids stay stable), but the reported count tracks predicates in use, so the
 * same log replayed into a fresh index reports the same number. */
static void pred_use(FactIndex *f, uint16_t id, int delta) {
    if (id == PRED_NONE || (size_t)id > f->pred_count) {
        return;
    }
    PredNode *n = f->pred_by_id[id - 1];
    if (delta > 0) {
        if (n->uses++ == 0) {
            f->preds_live++;
        }
    } else if (n->uses && --n->uses == 0) {
        f->preds_live--;
    }
}

/* ----- sorted id lists (the predicate -> records table) ------------------ */

static size_t id_lower_bound(const uint64_t *v, size_t n, uint64_t id) {
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) / 2);
        if (v[mid] < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static int idlist_add(uint64_t **v, size_t *n, size_t *cap, uint64_t id) {
    size_t pos = id_lower_bound(*v, *n, id);
    if (pos < *n && (*v)[pos] == id) {
        return 0; /* already present */
    }
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 4;
        uint64_t *t = realloc(*v, nc * sizeof(**v));
        if (!t) {
            return -1;
        }
        *v = t;
        *cap = nc;
    }
    if (pos < *n) {
        memmove(&(*v)[pos + 1], &(*v)[pos], (*n - pos) * sizeof(**v));
    }
    (*v)[pos] = id;
    (*n)++;
    return 0;
}

static void idlist_remove(uint64_t *v, size_t *n, uint64_t id) {
    size_t pos = id_lower_bound(v, *n, id);
    if (pos < *n && v[pos] == id) {
        memmove(&v[pos], &v[pos + 1], (*n - pos - 1) * sizeof(*v));
        (*n)--;
    }
}

/* ----- the subject / object tables --------------------------------------- */

/* Ordered by predicate then record, so a posting is found by binary search and
 * a predicate's run within a slot is contiguous. */
static int post_cmp(uint16_t ap, uint64_t ar, uint16_t bp, uint64_t br) {
    if (ap != bp) {
        return ap < bp ? -1 : 1;
    }
    if (ar != br) {
        return ar < br ? -1 : 1;
    }
    return 0;
}

static size_t post_lower_bound(const Post *p, size_t n, uint16_t pred,
                               uint64_t rec) {
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) / 2);
        if (post_cmp(p[mid].pred, p[mid].record_id, pred, rec) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static uint64_t str_hash64(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h ? h
             : 1; /* never 0, so a hash is distinguishable from an unset key */
}

/* Does this slot hold the given key? `str` non-NULL selects a string object,
 * whose hash may collide — so the stored text is compared too. */
static int slot_matches(const Slot *s, uint64_t key, const char *str) {
    if (s->key != key) {
        return 0;
    }
    if (str) {
        return s->obj_str && strcmp(s->obj_str, str) == 0;
    }
    return s->obj_str == NULL;
}

static Slot *tbl_find(const Table *t, uint64_t key, const char *str) {
    if (!t->cap) {
        return NULL;
    }
    size_t mask = t->cap - 1;
    size_t i = (size_t)mix64(key) & mask;
    for (size_t probe = 0; probe < t->cap; probe++) {
        Slot *s = &t->slots[(i + probe) & mask];
        if (s->state == SLOT_EMPTY) {
            return NULL; /* a never-used slot ends the probe chain */
        }
        if (s->state == SLOT_LIVE && slot_matches(s, key, str)) {
            return s;
        }
    }
    return NULL;
}

static int tbl_grow(Table *t) {
    size_t ncap = t->cap ? t->cap * 2 : 64;
    Slot *ns = calloc(ncap, sizeof(*ns));
    if (!ns) {
        return -1;
    }
    size_t mask = ncap - 1;
    for (size_t i = 0; i < t->cap; i++) {
        Slot *old = &t->slots[i];
        if (old->state != SLOT_LIVE) {
            continue; /* the rehash is also what drops the tombstones */
        }
        size_t j = (size_t)mix64(old->key) & mask;
        while (ns[j].state == SLOT_LIVE) {
            j = (j + 1) & mask;
        }
        ns[j] = *old;
    }
    free(t->slots);
    t->slots = ns;
    t->cap = ncap;
    t->used = t->live;
    return 0;
}

/* Slot for the key, creating it if absent. `str` is copied into a new slot. */
static Slot *tbl_put(Table *t, uint64_t key, const char *str) {
    if (!t->cap || ((t->used + 1) * 4) >= (t->cap * 3)) {
        if (tbl_grow(t) != 0) {
            return NULL;
        }
    }
    size_t mask = t->cap - 1;
    size_t i = (size_t)mix64(key) & mask;
    Slot *reuse = NULL;
    for (size_t probe = 0; probe < t->cap; probe++) {
        Slot *s = &t->slots[(i + probe) & mask];
        if (s->state == SLOT_LIVE) {
            if (slot_matches(s, key, str)) {
                return s;
            }
            continue;
        }
        if (s->state == SLOT_DEAD) {
            /* Remember the first tombstone but keep probing: a live slot for
             * this key may lie further along, and claiming the tombstone now
             * would duplicate it. */
            if (!reuse) {
                reuse = s;
            }
            continue;
        }
        Slot *slot = reuse ? reuse : s;
        char *copy = NULL;
        if (str) {
            size_t len = strlen(str);
            copy = malloc(len + 1);
            if (!copy) {
                return NULL;
            }
            memcpy(copy, str, len + 1);
        }
        if (slot->state == SLOT_EMPTY) {
            t->used++;
        }
        slot->key = key;
        slot->obj_str = copy;
        slot->posts = NULL;
        slot->n = 0;
        slot->cap = 0;
        slot->state = SLOT_LIVE;
        t->live++;
        return slot;
    }
    return NULL; /* table full: unreachable at a 3/4 load factor */
}

static void tbl_retire(Table *t, Slot *s) {
    free(s->posts);
    free(s->obj_str);
    s->posts = NULL;
    s->obj_str = NULL;
    s->n = 0;
    s->cap = 0;
    s->state = SLOT_DEAD;
    t->live--;
}

static int slot_add_post(Slot *s, uint16_t pred, uint64_t rec, int *added) {
    *added = 0;
    size_t pos = post_lower_bound(s->posts, s->n, pred, rec);
    if (pos < s->n && s->posts[pos].pred == pred &&
        s->posts[pos].record_id == rec) {
        return 0; /* idempotent */
    }
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 1; /* most slots hold exactly one */
        Post *t = realloc(s->posts, nc * sizeof(*t));
        if (!t) {
            return -1;
        }
        s->posts = t;
        s->cap = nc;
    }
    if (pos < s->n) {
        memmove(&s->posts[pos + 1], &s->posts[pos],
                (s->n - pos) * sizeof(*s->posts));
    }
    s->posts[pos].pred = pred;
    s->posts[pos].record_id = rec;
    s->n++;
    *added = 1;
    return 0;
}

static void slot_del_post(Table *t, Slot *s, uint16_t pred, uint64_t rec,
                          int *removed) {
    *removed = 0;
    size_t pos = post_lower_bound(s->posts, s->n, pred, rec);
    if (!(pos < s->n && s->posts[pos].pred == pred &&
          s->posts[pos].record_id == rec)) {
        return;
    }
    memmove(&s->posts[pos], &s->posts[pos + 1],
            (s->n - pos - 1) * sizeof(*s->posts));
    s->n--;
    *removed = 1;
    if (s->n == 0) {
        tbl_retire(t, s);
    }
}

/* ----- lifecycle --------------------------------------------------------- */

FactIndex *fact_index_create(void) { return calloc(1, sizeof(FactIndex)); }

static void tbl_free(Table *t) {
    for (size_t i = 0; i < t->cap; i++) {
        free(t->slots[i].posts);
        free(t->slots[i].obj_str);
    }
    free(t->slots);
}

void fact_index_free(FactIndex *f) {
    if (!f) {
        return;
    }
    tbl_free(&f->subj);
    tbl_free(&f->obj);
    for (size_t i = 0; i < FACT_MAX_PREDICATES; i++) {
        free(f->pred_recs[i]);
    }
    for (size_t i = 0; i < PRED_NBUCKETS; i++) {
        PredNode *n = f->pred_buckets[i];
        while (n) {
            PredNode *nx = n->next;
            free(n->name);
            free(n);
            n = nx;
        }
    }
    free(f->pred_by_id);
    free(f);
}

/* ----- mutation ---------------------------------------------------------- */

/* The object's table key: its id directly, or a string's hash. */
static int object_key(FactKind okind, uint64_t object_id,
                      const char *object_str, uint64_t *out_key,
                      const char **out_str) {
    if (okind == FACT_OBJ_ID) {
        *out_key = object_id;
        *out_str = NULL;
        return 0;
    }
    if (okind == FACT_OBJ_STRING && object_str) {
        *out_key = str_hash64(object_str);
        *out_str = object_str;
        return 0;
    }
    return -1;
}

int fact_index_add(FactIndex *f, uint64_t record_id, uint64_t subject,
                   const char *predicate, FactKind okind, uint64_t object_id,
                   const char *object_str) {
    if (!f) {
        return 0;
    }
    if (!predicate) {
        return -1;
    }
    uint64_t okey = 0;
    const char *ostr = NULL;
    if (object_key(okind, object_id, object_str, &okey, &ostr) != 0) {
        return -1;
    }
    uint16_t pred = pred_intern(f, predicate);
    if (pred == PRED_NONE) {
        return -1; /* refused, not silently unqueryable */
    }

    /* Both tables and the predicate list must land together: a fact present in
     * one and absent from another would answer some patterns and not others,
     * which is worse than not indexing it. So on any failure, undo. */
    Slot *ss = tbl_put(&f->subj, subject, NULL);
    if (!ss) {
        return -1;
    }
    int s_added = 0;
    if (slot_add_post(ss, pred, record_id, &s_added) != 0) {
        if (ss->n == 0) {
            tbl_retire(&f->subj, ss);
        }
        return -1;
    }
    Slot *os = tbl_put(&f->obj, okey, ostr);
    int o_added = 0;
    if (!os || slot_add_post(os, pred, record_id, &o_added) != 0) {
        if (os && os->n == 0) {
            tbl_retire(&f->obj, os);
        }
        if (s_added) {
            int dummy = 0;
            slot_del_post(&f->subj, ss, pred, record_id, &dummy);
        }
        return -1;
    }
    if (idlist_add(&f->pred_recs[pred - 1], &f->pred_n[pred - 1],
                   &f->pred_cap[pred - 1], record_id) != 0) {
        int dummy = 0;
        if (o_added) {
            slot_del_post(&f->obj, os, pred, record_id, &dummy);
        }
        if (s_added) {
            slot_del_post(&f->subj, ss, pred, record_id, &dummy);
        }
        return -1;
    }
    if (s_added) { /* a re-add of an identical fact changes no counters */
        pred_use(f, pred, +1);
        f->facts++;
    }
    return 0;
}

void fact_index_remove(FactIndex *f, uint64_t record_id, uint64_t subject,
                       const char *predicate, FactKind okind,
                       uint64_t object_id, const char *object_str) {
    if (!f || !predicate) {
        return;
    }
    uint64_t okey = 0;
    const char *ostr = NULL;
    if (object_key(okind, object_id, object_str, &okey, &ostr) != 0) {
        return;
    }
    uint16_t pred = pred_lookup(f, predicate);
    if (pred == PRED_NONE) {
        return; /* never indexed under this predicate */
    }
    int s_removed = 0;
    Slot *ss = tbl_find(&f->subj, subject, NULL);
    if (ss) {
        slot_del_post(&f->subj, ss, pred, record_id, &s_removed);
    }
    int o_removed = 0;
    Slot *os = tbl_find(&f->obj, okey, ostr);
    if (os) {
        slot_del_post(&f->obj, os, pred, record_id, &o_removed);
    }
    /* The predicate list is shared by every fact using that predicate, so it
     * only loses the record when this was its last fact under it — which the
     * subject side just told us. */
    if (s_removed) {
        idlist_remove(f->pred_recs[pred - 1], &f->pred_n[pred - 1], record_id);
        pred_use(f, pred, -1);
        f->facts--;
    }
    (void)o_removed;
}

/* ----- lookups ----------------------------------------------------------- */

/* Collect the record ids in `s` whose posting matches `pred` (PRED_NONE = any),
 * sorted and deduplicated. */
static int collect(const Slot *s, uint16_t pred, uint64_t **out,
                   size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    if (!s || s->n == 0) {
        return 0;
    }
    uint64_t *res = malloc(s->n * sizeof(*res));
    if (!res) {
        return -1;
    }
    size_t cnt = 0;
    size_t cap = s->n;
    for (size_t i = 0; i < s->n; i++) {
        if (pred != PRED_NONE && s->posts[i].pred != pred) {
            continue;
        }
        if (idlist_add(&res, &cnt, &cap, s->posts[i].record_id) != 0) {
            free(res);
            return -1;
        }
    }
    if (cnt == 0) {
        free(res);
        return 0;
    }
    *out = res;
    *out_n = cnt;
    return 0;
}

int fact_index_by_subject(const FactIndex *f, uint64_t subject,
                          const char *predicate, uint64_t **out,
                          size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    if (!f) {
        return 0;
    }
    uint16_t pred = PRED_NONE;
    if (predicate) {
        pred = pred_lookup(f, predicate);
        if (pred == PRED_NONE) {
            return 0; /* a predicate never seen matches nothing */
        }
    }
    return collect(tbl_find(&f->subj, subject, NULL), pred, out, out_n);
}

int fact_index_by_object(const FactIndex *f, FactKind okind, uint64_t object_id,
                         const char *object_str, const char *predicate,
                         uint64_t **out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    if (!f) {
        return 0;
    }
    uint64_t okey = 0;
    const char *ostr = NULL;
    if (object_key(okind, object_id, object_str, &okey, &ostr) != 0) {
        return -1;
    }
    uint16_t pred = PRED_NONE;
    if (predicate) {
        pred = pred_lookup(f, predicate);
        if (pred == PRED_NONE) {
            return 0;
        }
    }
    return collect(tbl_find(&f->obj, okey, ostr), pred, out, out_n);
}

size_t fact_index_predicate_facts(const FactIndex *f, const char *predicate) {
    if (!f || !predicate) {
        return 0;
    }
    uint16_t pred = pred_lookup(f, predicate);
    return pred == PRED_NONE ? 0 : f->pred_n[pred - 1];
}

int fact_index_by_predicate(const FactIndex *f, const char *predicate,
                            uint64_t **out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    if (!f || !predicate) {
        return 0;
    }
    uint16_t pred = pred_lookup(f, predicate);
    if (pred == PRED_NONE || f->pred_n[pred - 1] == 0) {
        return 0;
    }
    size_t n = f->pred_n[pred - 1];
    uint64_t *res = malloc(n * sizeof(*res));
    if (!res) {
        return -1;
    }
    memcpy(res, f->pred_recs[pred - 1], n * sizeof(*res));
    *out = res;
    *out_n = n;
    return 0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int fact_index_all_records(const FactIndex *f, uint64_t **out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    if (!f || f->facts == 0) {
        return 0;
    }
    /* A record carries at most one fact, so the per-predicate postings are
     * disjoint and their total is exactly f->facts — no dedup pass needed,
     * only a sort, which the callers of every other lookup here already
     * expect and which the inference job needs for a reproducible scan. */
    uint64_t *res = malloc(f->facts * sizeof(*res));
    if (!res) {
        return -1;
    }
    size_t n = 0;
    for (size_t i = 0; i < FACT_MAX_PREDICATES; i++) {
        for (size_t j = 0; j < f->pred_n[i]; j++) {
            if (n == f->facts) {
                /* The postings hold more than `facts` says: the two have
                 * drifted. Refuse rather than overrun — and say so, because
                 * the only other symptom is inference quietly never running
                 * again, which looks exactly like a settled corpus. */
                LOG_WARN("fact index: postings exceed the fact count (%zu); "
                         "enumeration refused",
                         f->facts);
                free(res);
                return -1;
            }
            res[n++] = f->pred_recs[i][j];
        }
    }
    qsort(res, n, sizeof(*res), cmp_u64);
    *out = res;
    *out_n = n;
    return 0;
}

/* ----- introspection ----------------------------------------------------- */

size_t fact_index_facts(const FactIndex *f) { return f ? f->facts : 0; }

size_t fact_index_predicates(const FactIndex *f) {
    return f ? f->preds_live : 0;
}

static size_t tbl_bytes(const Table *t) {
    size_t total = t->cap * sizeof(Slot);
    for (size_t i = 0; i < t->cap; i++) {
        total += t->slots[i].cap * sizeof(Post);
        if (t->slots[i].obj_str) {
            total += strlen(t->slots[i].obj_str) + 1;
        }
    }
    return total;
}

size_t fact_index_bytes(const FactIndex *f) {
    if (!f) {
        return 0;
    }
    size_t total = sizeof(*f);
    total += tbl_bytes(&f->subj);
    total += tbl_bytes(&f->obj);
    for (size_t i = 0; i < FACT_MAX_PREDICATES; i++) {
        total += f->pred_cap[i] * sizeof(uint64_t);
    }
    total += f->pred_by_id_cap * sizeof(PredNode *);
    for (size_t i = 0; i < PRED_NBUCKETS; i++) {
        for (const PredNode *n = f->pred_buckets[i]; n; n = n->next) {
            total += sizeof(*n) + strlen(n->name) + 1;
        }
    }
    return total;
}
