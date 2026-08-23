/* Deterministic inference (ROADMAP 5.3): the three closures, computed purely.
 *
 * Two tables carry the whole pass. `Seen` answers "does this triple already
 * exist?" — over the input snapshot first, then over conclusions as they are
 * drawn, which is what makes the pass idempotent and what stops a cycle without
 * a cycle detector. `Adj` answers "which facts have subject b and predicate
 * p?", which is the join transitivity needs and the only reason this is not a
 * quadratic scan.
 *
 * Both are open-addressed with linear probing, sized to a power of two, in the
 * shape the storage-side indexes use. Neither outlives infer_run. */
#include "aegisdb/inference.h"

#include <stdlib.h>
#include <string.h>

#include "aegisdb/hash_mix.h"

/* FNV-1a, as in fact_index.c. Never 0, so a hash is distinguishable from an
 * unset key. */
static uint64_t str_hash64(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; s && *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

/* ----- the triple set --------------------------------------------------- */

typedef struct {
    uint64_t subject;
    const char *predicate; /* borrowed */
    FactKind okind;
    uint64_t oid;
    const char *ostr; /* borrowed */
    /* Index into the output buffer for a triple this pass concluded, or
     * SEEN_INPUT for one that was already a fact. Keeping it lets a second
     * route to the same triple compare itself against the route already
     * recorded, instead of being dropped along with its provenance. */
    size_t out_idx;
    uint8_t used;
} SeenSlot;

#define SEEN_INPUT ((size_t)-1)

typedef struct {
    SeenSlot *slots;
    size_t cap; /* power of two */
    size_t n;
} Seen;

static uint64_t triple_hash(uint64_t subject, const char *predicate,
                            FactKind okind, uint64_t oid, const char *ostr) {
    uint64_t h = mix64(subject) ^ str_hash64(predicate);
    h ^= (okind == FACT_OBJ_ID) ? mix64(oid) : str_hash64(ostr);
    return mix64(h);
}

static int triple_eq(const SeenSlot *s, uint64_t subject, const char *predicate,
                     FactKind okind, uint64_t oid, const char *ostr) {
    if (s->subject != subject || s->okind != okind) {
        return 0;
    }
    if (strcmp(s->predicate, predicate) != 0) {
        return 0;
    }
    if (okind == FACT_OBJ_ID) {
        return s->oid == oid;
    }
    /* Objects are not interned, so the hash can collide; the stored text is
     * compared on a hit, exactly as the object table does. */
    return s->ostr && ostr && strcmp(s->ostr, ostr) == 0;
}

static int seen_grow(Seen *t) {
    size_t ncap = t->cap ? t->cap * 2 : 64;
    SeenSlot *ns = calloc(ncap, sizeof(*ns));
    if (!ns) {
        return -1;
    }
    for (size_t i = 0; i < t->cap; i++) {
        if (!t->slots[i].used) {
            continue;
        }
        const SeenSlot *o = &t->slots[i];
        size_t j = (size_t)(triple_hash(o->subject, o->predicate, o->okind,
                                        o->oid, o->ostr) &
                            (ncap - 1));
        while (ns[j].used) {
            j = (j + 1) & (ncap - 1);
        }
        ns[j] = *o;
    }
    free(t->slots);
    t->slots = ns;
    t->cap = ncap;
    return 0;
}

/* The slot holding this triple, or NULL. */
static SeenSlot *seen_find(const Seen *t, uint64_t subject,
                           const char *predicate, FactKind okind, uint64_t oid,
                           const char *ostr) {
    if (!t->cap) {
        return NULL;
    }
    size_t i = (size_t)(triple_hash(subject, predicate, okind, oid, ostr) &
                        (t->cap - 1));
    while (t->slots[i].used) {
        if (triple_eq(&t->slots[i], subject, predicate, okind, oid, ostr)) {
            return &t->slots[i];
        }
        i = (i + 1) & (t->cap - 1);
    }
    return NULL;
}

/* Insert if absent. Returns 1 if newly inserted, 0 if already present, -1 on
 * allocation failure. */
static int seen_put(Seen *t, uint64_t subject, const char *predicate,
                    FactKind okind, uint64_t oid, const char *ostr,
                    size_t out_idx) {
    if (t->n * 4 >= t->cap * 3 && seen_grow(t) != 0) {
        return -1;
    }
    size_t i = (size_t)(triple_hash(subject, predicate, okind, oid, ostr) &
                        (t->cap - 1));
    while (t->slots[i].used) {
        if (triple_eq(&t->slots[i], subject, predicate, okind, oid, ostr)) {
            return 0;
        }
        i = (i + 1) & (t->cap - 1);
    }
    t->slots[i].subject = subject;
    t->slots[i].predicate = predicate;
    t->slots[i].okind = okind;
    t->slots[i].oid = oid;
    t->slots[i].ostr = ostr;
    t->slots[i].out_idx = out_idx;
    t->slots[i].used = 1;
    t->n++;
    return 1;
}

/* ----- the (subject, predicate) -> input indices adjacency --------------- */

typedef struct {
    uint64_t subject;
    const char *predicate; /* borrowed */
    size_t *idx;           /* indices into the caller's facts array */
    size_t n;
    size_t cap;
    uint8_t used;
} AdjSlot;

typedef struct {
    AdjSlot *slots;
    size_t cap;
    size_t n;
} Adj;

static uint64_t sp_hash(uint64_t subject, const char *predicate) {
    return mix64(mix64(subject) ^ str_hash64(predicate));
}

static void adj_free(Adj *a) {
    for (size_t i = 0; i < a->cap; i++) {
        free(a->slots[i].idx);
    }
    free(a->slots);
    memset(a, 0, sizeof(*a));
}

static int adj_grow(Adj *a) {
    size_t ncap = a->cap ? a->cap * 2 : 64;
    AdjSlot *ns = calloc(ncap, sizeof(*ns));
    if (!ns) {
        return -1;
    }
    for (size_t i = 0; i < a->cap; i++) {
        if (!a->slots[i].used) {
            continue;
        }
        size_t j =
            (size_t)(sp_hash(a->slots[i].subject, a->slots[i].predicate) &
                     (ncap - 1));
        while (ns[j].used) {
            j = (j + 1) & (ncap - 1);
        }
        ns[j] = a->slots[i];
    }
    free(a->slots);
    a->slots = ns;
    a->cap = ncap;
    return 0;
}

static int adj_add(Adj *a, uint64_t subject, const char *predicate,
                   size_t idx) {
    if (a->n * 4 >= a->cap * 3 && adj_grow(a) != 0) {
        return -1;
    }
    size_t i = (size_t)(sp_hash(subject, predicate) & (a->cap - 1));
    while (a->slots[i].used) {
        if (a->slots[i].subject == subject &&
            strcmp(a->slots[i].predicate, predicate) == 0) {
            break;
        }
        i = (i + 1) & (a->cap - 1);
    }
    AdjSlot *s = &a->slots[i];
    if (!s->used) {
        s->used = 1;
        s->subject = subject;
        s->predicate = predicate;
        a->n++;
    }
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 2;
        size_t *ni = realloc(s->idx, nc * sizeof(*ni));
        if (!ni) {
            return -1;
        }
        s->idx = ni;
        s->cap = nc;
    }
    s->idx[s->n++] = idx;
    return 0;
}

/* The facts with this subject and predicate, or NULL. */
static const AdjSlot *adj_get(const Adj *a, uint64_t subject,
                              const char *predicate) {
    if (!a->cap) {
        return NULL;
    }
    size_t i = (size_t)(sp_hash(subject, predicate) & (a->cap - 1));
    while (a->slots[i].used) {
        if (a->slots[i].subject == subject &&
            strcmp(a->slots[i].predicate, predicate) == 0) {
            return &a->slots[i];
        }
        i = (i + 1) & (a->cap - 1);
    }
    return NULL;
}

/* ----- the pass ---------------------------------------------------------- */

typedef struct {
    InferConclusion *items;
    size_t n;
    size_t cap;
} OutBuf;

static int out_push(OutBuf *o, const InferConclusion *c) {
    if (o->n == o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 16;
        InferConclusion *ni = realloc(o->items, nc * sizeof(*ni));
        if (!ni) {
            return -1;
        }
        o->items = ni;
        o->cap = nc;
    }
    o->items[o->n++] = *c;
    return 0;
}

/* The deepest premise, and the confidence product. Depth is *not* incremented
 * here: the caller compares this against max_depth first, because incrementing
 * a premise already at UINT16_MAX would wrap to 0 and produce a conclusion
 * indistinguishable from an asserted fact — after which the chain cap would
 * never bite again. */
static void route_of(InferConclusion *c, const InferFact *a, const InferFact *b,
                     float floor, uint16_t *deepest) {
    uint16_t d = a->depth;
    float conf = a->confidence;
    c->premises[0] = a->record_id;
    c->premise_count = 1;
    if (b) {
        if (b->depth > d) {
            d = b->depth;
        }
        conf *= b->confidence;
        c->premises[1] = b->record_id;
        c->premise_count = 2;
    }
    *deepest = d;
    /* The floor can raise a conclusion above its premises. That is deliberate
     * (design §8: max(product, floor)) — it keeps a long chain from decaying
     * out of ranked results — but it does mean confidence is not monotonic
     * along a chain, and it is a heuristic rather than a probability. */
    c->confidence = conf < floor ? floor : conf;
}

/* A total order over the routes to one triple, so which route gets recorded
 * does not depend on the order the caller happened to pass its facts in. The
 * provenance ends up in a durable record, so "whichever we saw first" would
 * make the log a function of scan order. */
static int route_is_better(const InferConclusion *cand,
                           const InferConclusion *cur) {
    if (cand->premises[0] != cur->premises[0]) {
        return cand->premises[0] < cur->premises[0];
    }
    uint64_t a = cand->premise_count > 1 ? cand->premises[1] : 0;
    uint64_t b = cur->premise_count > 1 ? cur->premises[1] : 0;
    if (a != b) {
        return a < b;
    }
    return (int)cand->rule < (int)cur->rule;
}

/* Offer one candidate conclusion to the pass. Returns 1 if it was recorded, 0
 * if the triple was already known (as an input fact, or by another route), or
 * -1 on allocation failure. `*stop` is set when a cap ends the pass. */
static int offer(OutBuf *out, Seen *seen, const InferConclusion *c,
                 uint16_t deepest, uint16_t max_depth, size_t max_conc,
                 int *stop, int *truncated) {
    /* Compared before the increment, so a premise at UINT16_MAX cannot wrap. */
    if (deepest >= max_depth) {
        return 0;
    }
    SeenSlot *slot = seen_find(seen, c->subject, c->predicate, c->object_kind,
                               c->object_id, c->object_str);
    if (slot) {
        /* Already a fact: nothing to record, and no provenance to improve. */
        if (slot->out_idx == SEEN_INPUT) {
            return 0;
        }
        InferConclusion *cur = &out->items[slot->out_idx];
        if (route_is_better(c, cur)) {
            uint16_t keep_depth = cur->depth;
            *cur = *c;
            cur->depth = keep_depth < (uint16_t)(deepest + 1)
                             ? keep_depth
                             : (uint16_t)(deepest + 1);
        }
        return 0;
    }
    /* Genuinely new, so the output cap applies — and only here, so a pass that
     * meets the cap and then sees nothing but duplicates is not reported as
     * having deferred work it does not have. */
    if (max_conc && out->n >= max_conc) {
        *truncated = 1;
        *stop = 1;
        return 0;
    }
    InferConclusion rec = *c;
    rec.depth = (uint16_t)(deepest + 1);
    if (out_push(out, &rec) != 0) {
        return -1;
    }
    if (seen_put(seen, rec.subject, rec.predicate, rec.object_kind,
                 rec.object_id, rec.object_str, out->n - 1) < 0) {
        return -1;
    }
    return 1;
}

int infer_run(const InferFact *facts, size_t nfacts,
              const PredicateRegistry *reg, const InferOpts *opts,
              InferResult *out) {
    memset(out, 0, sizeof(*out));
    if (!reg || nfacts == 0) {
        return 0; /* nothing declared, or nothing to declare it about */
    }

    uint16_t max_depth =
        (opts && opts->max_depth) ? opts->max_depth : INFER_DEFAULT_MAX_DEPTH;
    size_t max_conc = opts ? opts->max_conclusions : 0;
    size_t max_cand = (opts && opts->max_candidates)
                          ? opts->max_candidates
                          : INFER_DEFAULT_MAX_CANDIDATES;
    float floor = (opts && opts->confidence_floor > 0.0F)
                      ? opts->confidence_floor
                      : INFER_DEFAULT_CONFIDENCE_FLOOR;
    size_t start = (opts && nfacts) ? opts->start_index % nfacts : 0;

    Seen seen = {0};
    Adj adj = {0};
    OutBuf out_buf = {0};
    size_t cands = 0;
    int stop = 0;
    int rc = -1;

    /* Phase 1 is linear and unbudgeted: it is what makes dedup possible, so
     * skipping part of it would re-derive facts that already exist. */
    for (size_t i = 0; i < nfacts; i++) {
        const InferFact *f = &facts[i];
        if (seen_put(&seen, f->subject, f->predicate, f->object_kind,
                     f->object_id, f->object_str, SEEN_INPUT) < 0) {
            goto done;
        }
        /* Only id-objects can be joined or reversed, and only those predicates
         * can carry the three properties — so a literal-valued fact is indexed
         * for dedup above and skipped here. */
        if (f->object_kind == FACT_OBJ_ID &&
            adj_add(&adj, f->subject, f->predicate, i) != 0) {
            goto done;
        }
    }

    /* Phase 2 is budgeted, and starts wherever the caller asked. A closed
     * corpus offers the same candidates every pass and keeps none of them, so
     * a budget that counted *kept* conclusions would never fire and the pass
     * would grow with the corpus; counting candidates is what actually bounds
     * a tick. Rotating the start is what keeps a budgeted pass from examining
     * the same prefix forever and never reaching the rest. */
    for (size_t k = 0; k < nfacts && !stop; k++) {
        const InferFact *f = &facts[(start + k) % nfacts];
        if (f->object_kind != FACT_OBJ_ID) {
            continue;
        }
        const PredicateSpec *spec = predicate_registry_get(reg, f->predicate);
        if (!spec) {
            continue; /* undeclared: nothing to conclude from it */
        }

        if (spec->symmetric) {
            if (++cands > max_cand) {
                out->truncated = 1;
                stop = 1;
                break;
            }
            InferConclusion c = {0};
            uint16_t deepest = 0;
            c.rule = DERIV_SYMMETRIC;
            c.subject = f->object_id;
            c.predicate = f->predicate;
            c.object_kind = FACT_OBJ_ID;
            c.object_id = f->subject;
            route_of(&c, f, NULL, floor, &deepest);
            if (offer(&out_buf, &seen, &c, deepest, max_depth, max_conc, &stop,
                      &out->truncated) < 0) {
                goto done;
            }
        }

        if (spec->inverse_of && !stop) {
            if (++cands > max_cand) {
                out->truncated = 1;
                stop = 1;
                break;
            }
            InferConclusion c = {0};
            uint16_t deepest = 0;
            c.rule = DERIV_INVERSE;
            c.subject = f->object_id;
            c.predicate = spec->inverse_of; /* borrowed from the registry */
            c.object_kind = FACT_OBJ_ID;
            c.object_id = f->subject;
            route_of(&c, f, NULL, floor, &deepest);
            if (offer(&out_buf, &seen, &c, deepest, max_depth, max_conc, &stop,
                      &out->truncated) < 0) {
                goto done;
            }
        }

        if (spec->transitive && !stop) {
            const AdjSlot *next = adj_get(&adj, f->object_id, f->predicate);
            for (size_t j = 0; next && j < next->n && !stop; j++) {
                if (++cands > max_cand) {
                    /* `stop` and not a bare break: this is the inner join loop,
                     * and breaking only it would let the outer scan carry on
                     * spending a budget that is already gone. */
                    out->truncated = 1;
                    stop = 1;
                    break;
                }
                const InferFact *g = &facts[next->idx[j]];
                InferConclusion c = {0};
                uint16_t deepest = 0;
                c.rule = DERIV_TRANSITIVE;
                c.subject = f->subject;
                c.predicate = f->predicate;
                c.object_kind = FACT_OBJ_ID;
                c.object_id = g->object_id;
                route_of(&c, f, g, floor, &deepest);
                if (offer(&out_buf, &seen, &c, deepest, max_depth, max_conc,
                          &stop, &out->truncated) < 0) {
                    goto done;
                }
            }
        }
    }

    out->items = out_buf.items;
    out->n = out_buf.n;
    out->candidates_examined = cands;
    out_buf.items = NULL;
    rc = 0;
done:
    free(out_buf.items);
    free(seen.slots);
    adj_free(&adj);
    if (rc != 0) {
        memset(out, 0, sizeof(*out));
    }
    return rc;
}

void infer_result_free(InferResult *r) {
    if (!r) {
        return;
    }
    free(r->items);
    memset(r, 0, sizeof(*r));
}
