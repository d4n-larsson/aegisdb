/* Query engine — the inference job (ROADMAP 5.3, PR 3).
 *
 * One step on the maintenance tick: read the live fact set, hand it to the pure
 * closures in inference.c, and write what comes back as records. Everything
 * here is the part that could not live in the pure module — where the facts
 * come from, which namespace they belong to, and how a conclusion becomes a
 * durable, provenance-linked record.
 *
 * Three properties this file is responsible for, none of which infer_run can
 * check for itself (docs/inference-design.md §5, §7):
 *
 *   - **Primary only.** A follower that derived locally would append frames its
 *     primary never sent and desync a log that must stay byte-identical.
 *     Derived records reach a replica through the stream like any other.
 *   - **One namespace at a time.** The fact indexes are server-wide, so a pass
 *     over all of them would join a premise from one tenant to a premise from
 *     another and write a record that exists in neither.
 *   - **Bounded.** The caps come from config; the scan start rotates, so a
 *     budgeted pass does not examine the same prefix forever.
 */
#include "aegisdb/query_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aegisdb/inference.h"
#include "aegisdb/logging.h"

#include "qe_internal.h"

/* A loaded fact-bearing record, kept alive for the pass: infer_run borrows the
 * predicate and object strings, so the records must outlive the result. */
typedef struct {
    MemoryRecord rec;
    const char *ns; /* borrowed from rec.agent_id; "" when unnamespaced */
} LoadedFact;

static int cmp_by_ns(const void *a, const void *b) {
    const LoadedFact *x = a;
    const LoadedFact *y = b;
    int c = strcmp(x->ns, y->ns);
    if (c != 0) {
        return c;
    }
    /* Stable within a namespace, so a pass is reproducible and the rotating
     * scan offset means the same thing from one tick to the next. */
    return (x->rec.id > y->rec.id) - (x->rec.id < y->rec.id);
}

static void free_loaded(LoadedFact *f, size_t n) {
    for (size_t i = 0; i < n; i++) {
        record_free(&f[i].rec);
    }
    free(f);
}

/* Load every fact-bearing record, grouped by namespace. The ids come from the
 * fact index rather than a corpus walk, so this is O(facts) reads — but the
 * records themselves are needed regardless, because depth and confidence live
 * on the record and not in the index. */
static int load_facts(AegisDB *db, LoadedFact **out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    uint64_t *ids = NULL;
    size_t n_ids = 0;
    pthread_rwlock_rdlock(&db->index_lock);
    int rv = fact_index_all_records(db->facts, &ids, &n_ids);
    pthread_rwlock_unlock(&db->index_lock);
    if (rv != 0) {
        return -1;
    }
    if (n_ids == 0) {
        free(ids);
        return 0;
    }
    LoadedFact *loaded = calloc(n_ids, sizeof(*loaded));
    if (!loaded) {
        free(ids);
        return -1;
    }
    size_t n = 0;
    for (size_t i = 0; i < n_ids; i++) {
        MemoryRecord r;
        /* A tombstone between the index read and here is ordinary: skip it.
         * The index is a hint about what to load, not a guarantee it is still
         * there. */
        if (qe_get(db, ids[i], NULL, &r) != AEGIS_OK) {
            continue;
        }
        if (r.fact.kind == FACT_NONE) {
            record_free(&r);
            continue;
        }
        loaded[n].rec = r;
        loaded[n].ns = r.agent_id ? r.agent_id : "";
        n++;
    }
    free(ids);
    qsort(loaded, n, sizeof(*loaded), cmp_by_ns);
    *out = loaded;
    *out_n = n;
    return 0;
}

/* Prose for a derived record. A conclusion that shows up in a search result
 * should be readable by whoever finds it, and `insert` refuses an empty
 * payload — so this is required, not decoration. */
static void conclusion_text(char *buf, size_t cap, const InferConclusion *c,
                            const InferRoute *rt) {
    static const char *const RULE[] = {"", "transitive", "symmetric",
                                       "inverse"};
    const char *rule = (rt->rule >= 1 && rt->rule <= 3) ? RULE[rt->rule] : "?";
    if (c->object_kind == FACT_OBJ_ID) {
        snprintf(buf, cap, "#%llu %s #%llu (derived: %s)",
                 (unsigned long long)c->subject, c->predicate,
                 (unsigned long long)c->object_id, rule);
    } else {
        snprintf(buf, cap, "#%llu %s \"%s\" (derived: %s)",
                 (unsigned long long)c->subject, c->predicate,
                 c->object_str ? c->object_str : "", rule);
    }
}

/* Write one conclusion as a record, then link it to its premises. Returns 0 on
 * success, -1 if the record could not be written. */
static int write_conclusion(AegisDB *db, const InferConclusion *c,
                            const char *ns) {
    MemoryRecord in;
    record_init(&in);
    in.type = MEM_SEMANTIC;
    in.confidence = c->confidence;
    char text[512];
    conclusion_text(text, sizeof text, c, &c->routes[0]);
    in.data = text; /* borrowed; qe_insert copies */
    in.data_len = strlen(text);
    if (ns && *ns) {
        in.agent_id = (char *)ns; /* borrowed; qe_insert copies */
    }
    int rv = -1;
    if (record_set_fact(&in, c->object_kind, c->subject, c->predicate,
                        c->object_id, c->object_str) != 0) {
        goto out;
    }
    for (size_t i = 0; i < c->route_count; i++) {
        const InferRoute *rt = &c->routes[i];
        if (record_add_route(&in, rt->rule, rt->depth, rt->premises,
                             rt->premise_count) < 0) {
            goto out;
        }
    }

    MemoryRecord written;
    aegis_status_t st = qe_insert(db, &in, NULL, 0, &written);
    if (st != AEGIS_OK) {
        LOG_WARN("inference: could not write a conclusion (%d)", (int)st);
        goto out;
    }
    /* The edges are what a walk uses; the routes on the record are what
     * survives a premise's tombstone. Both, on purpose — see §3. A premise
     * tombstoned since the pass read it makes relate fail, which is harmless:
     * the routes still name it, and retraction will act on that. */
    for (size_t i = 0; i < c->route_count; i++) {
        for (size_t j = 0; j < c->routes[i].premise_count; j++) {
            (void)qe_relate(db, written.id, c->routes[i].premises[j],
                            "derived_from", ns && *ns ? ns : NULL);
        }
    }
    record_free(&written);
    rv = 0;
out:
    in.data = NULL; /* borrowed */
    in.data_len = 0;
    in.agent_id = NULL; /* borrowed */
    record_free(&in);
    return rv;
}

/* One namespace's pass. Returns the number of conclusions written, and sets
 * *deferred when a cap stopped it short. */
static size_t run_namespace(AegisDB *db, const LoadedFact *facts, size_t n,
                            const char *ns, size_t budget, int *deferred) {
    InferFact *in = calloc(n, sizeof(*in));
    if (!in) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        const MemoryRecord *r = &facts[i].rec;
        in[i].record_id = r->id;
        in[i].subject = r->fact.subject;
        in[i].predicate = r->fact.predicate;
        in[i].object_kind = r->fact.kind;
        in[i].object_id = r->fact.object_id;
        in[i].object_str = r->fact.object_str;
        in[i].depth = derivation_depth(&r->derivation);
        in[i].confidence = r->confidence;
    }

    InferOpts opts = {0};
    opts.max_depth = (uint16_t)db->config.inference_max_depth;
    opts.max_conclusions = budget;
    opts.max_candidates = db->config.inference_max_candidates;
    opts.confidence_floor = db->config.inference_confidence_floor;
    /* Rotate the scan so a budgeted pass eventually reaches every fact rather
     * than re-examining the same prefix each tick. */
    opts.start_index =
        (size_t)atomic_load_explicit(&db->infer_cursor, memory_order_relaxed);

    InferResult res;
    if (infer_run(in, n, db->predicates, &opts, &res) != 0) {
        free(in);
        return 0;
    }
    size_t written = 0;
    for (size_t i = 0; i < res.n; i++) {
        if (write_conclusion(db, &res.items[i], ns) == 0) {
            written++;
        }
    }
    if (res.truncated) {
        *deferred = 1;
        atomic_fetch_add_explicit(&db->infer_cursor,
                                  (uint_fast64_t)res.candidates_examined,
                                  memory_order_relaxed);
    }
    infer_result_free(&res);
    free(in);
    return written;
}

size_t db_inference_step(AegisDB *db) {
    if (!db->config.inference || !db->predicates || !db->facts) {
        return 0; /* nothing configured to reason with */
    }
    /* A follower applies what its primary already decided. Deriving here would
     * append frames the primary never sent and desync a byte-identical log —
     * the same reason sweep and compaction are skipped on a replica. */
    if (db->config.read_only) {
        return 0;
    }

    LoadedFact *facts = NULL;
    size_t n = 0;
    if (load_facts(db, &facts, &n) != 0 || n == 0) {
        free_loaded(facts, n);
        return 0;
    }

    uint64_t t0 = db_now_ms();
    size_t budget = db->config.inference_max_derived;
    size_t total = 0;
    int deferred = 0;
    /* Grouped by namespace, so no rule ever joins two tenants' facts. */
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && strcmp(facts[j].ns, facts[i].ns) == 0) {
            j++;
        }
        size_t left = budget > total ? budget - total : 0;
        if (left == 0) {
            deferred = 1; /* namespaces after this one wait for the next tick */
            break;
        }
        total +=
            run_namespace(db, &facts[i], j - i, facts[i].ns, left, &deferred);
        i = j;
    }
    free_loaded(facts, n);

    atomic_store_explicit(&db->infer_last_ms, db_now_ms() - t0,
                          memory_order_relaxed);
    atomic_store_explicit(&db->infer_deferred, deferred ? 1 : 0,
                          memory_order_relaxed);
    if (total) {
        atomic_fetch_add_explicit(&db->derived_total, (uint_fast64_t)total,
                                  memory_order_relaxed);
        LOG_DEBUG("inference: wrote %zu conclusion(s)%s", total,
                  deferred ? " (a cap deferred the rest)" : "");
    }
    return total;
}
