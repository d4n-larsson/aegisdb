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
            aegis_status_t rst =
                qe_relate(db, written.id, c->routes[i].premises[j],
                          "derived_from", ns && *ns ? ns : NULL);
            /* NOT_FOUND means the premise was tombstoned between the pass
             * reading it and this call. That is not benign, as an earlier
             * version of this comment claimed: the conclusion is now written
             * already unjustified, and the premise's own qe_delete — the only
             * thing that enqueues — ran before this record existed, so nothing
             * would ever judge it. Queue it here instead, and let the drain
             * decide from its routes; another route may well still hold. */
            if (rst == AEGIS_ERR_NOT_FOUND) {
                db_retract_enqueue_id(db, written.id);
            } else if (rst != AEGIS_OK) {
                LOG_WARN("inference: record %llu could not link premise %llu "
                         "(%d); its routes still name it",
                         (unsigned long long)written.id,
                         (unsigned long long)c->routes[i].premises[j],
                         (int)rst);
            }
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

/* ----- contradiction detection (ROADMAP 5.3 §4.3) ------------------------ */

/* Does this record already record a conflict with `peer`? The pass recomputes
 * from scratch every tick, so without this it would re-log the same
 * contradiction forever. */
static int already_conflicting(const MemoryRecord *r, uint64_t peer) {
    for (size_t i = 0; i < r->rel_count; i++) {
        if (r->relationships[i].to_id == peer && r->relationships[i].kind &&
            strcmp(r->relationships[i].kind, "conflicts_with") == 0) {
            return 1;
        }
    }
    return 0;
}

/* Record that two records contradict each other. Both directions, because a
 * contradiction is not a claim one of them makes about the other — a reader
 * arriving at either should see it. */
static void note_conflict(AegisDB *db, const LoadedFact *a, const LoadedFact *b,
                          const char *ns, const char *why) {
    if (already_conflicting(&a->rec, b->rec.id) &&
        already_conflicting(&b->rec, a->rec.id)) {
        return;
    }
    const char *scope = (ns && *ns) ? ns : NULL;
    aegis_status_t sa =
        qe_relate(db, a->rec.id, b->rec.id, "conflicts_with", scope);
    aegis_status_t sb =
        qe_relate(db, b->rec.id, a->rec.id, "conflicts_with", scope);
    if (sa == AEGIS_OK || sb == AEGIS_OK) {
        LOG_WARN("inference: records %llu and %llu contradict each other (%s)",
                 (unsigned long long)a->rec.id, (unsigned long long)b->rec.id,
                 why);
        return;
    }
    /* Neither edge stuck — the record is at MAX_RELATIONSHIPS, or a peer was
     * tombstoned since the snapshot. already_conflicting reads the record's own
     * array, so nothing was recorded and the next pass will try again; at WARN
     * that would be per-tick spam forever on a hot subject. The contradiction
     * is still counted, so `conflicts` does not go quiet about it. */
    LOG_DEBUG("inference: could not link %llu and %llu (%s): %d/%d",
              (unsigned long long)a->rec.id, (unsigned long long)b->rec.id, why,
              (int)sa, (int)sb);
}

/* Order a namespace's facts so equal (subject, predicate) sit together, which
 * turns cardinality checking into a linear scan of runs. */
static int cmp_subject_pred(const void *x, const void *y) {
    const LoadedFact *const *pa = x;
    const LoadedFact *const *pb = y;
    const Fact *a = &(*pa)->rec.fact;
    const Fact *b = &(*pb)->rec.fact;
    if (a->subject != b->subject) {
        return a->subject < b->subject ? -1 : 1;
    }
    int c = strcmp(a->predicate, b->predicate);
    if (c) {
        return c;
    }
    return ((*pa)->rec.id > (*pb)->rec.id) - ((*pa)->rec.id < (*pb)->rec.id);
}

/* Do two facts about the same subject assert different things? */
static int objects_differ(const Fact *a, const Fact *b) {
    if (a->kind != b->kind) {
        return 1;
    }
    if (a->kind == FACT_OBJ_ID) {
        return a->object_id != b->object_id;
    }
    return !a->object_str || !b->object_str ||
           strcmp(a->object_str, b->object_str) != 0;
}

/* Are these two predicates declared mutually exclusive? The registry validates
 * the declaration, so checking one direction would be enough — checking both
 * costs nothing and does not depend on that staying true. */
static int mutually_exclusive(const PredicateSpec *a, const PredicateSpec *b) {
    for (size_t i = 0; a->mutex_with && i < a->mutex_count; i++) {
        if (strcmp(a->mutex_with[i], b->name) == 0) {
            return 1;
        }
    }
    for (size_t i = 0; b->mutex_with && i < b->mutex_count; i++) {
        if (strcmp(b->mutex_with[i], a->name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Facts about one subject, beyond which the pairwise mutex scan is skipped. A
 * subject with this many assertions is a data-modelling problem, and O(k^2) on
 * it would be a tick-duration problem. */
#define CONFLICT_MAX_PER_SUBJECT 256

/* Find every contradiction in one namespace and return how many there are.
 *
 * Reported, never resolved: nothing is tombstoned, nothing is reranked, no
 * record is marked untrustworthy. Deciding which of two conflicting facts is
 * right needs to know which is newer, which source is better, or what the world
 * is actually like — none of which this layer knows. What it can guarantee is
 * that the contradiction is *found*, deterministically and with no model call,
 * which is the thing a human or an extractor cannot do reliably. */
static size_t find_conflicts(AegisDB *db, const LoadedFact *facts, size_t n,
                             const char *ns) {
    const LoadedFact **by_sp = malloc(n * sizeof(*by_sp));
    if (!by_sp) {
        return 0;
    }
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (facts[i].rec.fact.kind != FACT_NONE &&
            facts[i].rec.fact.predicate) {
            by_sp[m++] = &facts[i];
        }
    }
    qsort(by_sp, m, sizeof(*by_sp), cmp_subject_pred);

    size_t found = 0;
    for (size_t i = 0; i < m;) {
        /* One subject's run. */
        size_t s_end = i;
        while (s_end < m &&
               by_sp[s_end]->rec.fact.subject == by_sp[i]->rec.fact.subject) {
            s_end++;
        }
        size_t k = s_end - i;
        /* One cap for the whole subject, not just the pairwise half: the
         * cardinality scan below is linear, but every pair it reports costs
         * two log appends and two write-lock acquisitions, and a subject with
         * more assertions than this is a data-modelling problem rather than
         * something to spend a tick on. */
        if (k > CONFLICT_MAX_PER_SUBJECT) {
            LOG_WARN("inference: subject %llu has %zu facts; skipping its "
                     "contradiction scan",
                     (unsigned long long)by_sp[i]->rec.fact.subject, k);
            i = s_end;
            continue;
        }

        /* cardinality: one — two live values for a single-valued predicate.
         *
         * Linked to the run's first record rather than pairwise. p distinct
         * values would otherwise be p(p-1)/2 pairs, each two fsyncing writes,
         * inside a tick that budgets everything else — and a star still leaves
         * every conflicting record carrying an edge, so a reader arriving at
         * any of them sees the contradiction. */
        for (size_t j = i; j < s_end;) {
            size_t p_end = j;
            while (p_end < s_end && strcmp(by_sp[p_end]->rec.fact.predicate,
                                           by_sp[j]->rec.fact.predicate) == 0) {
                p_end++;
            }
            const PredicateSpec *spec = predicate_registry_get(
                db->predicates, by_sp[j]->rec.fact.predicate);
            if (spec && spec->single_valued) {
                for (size_t y = j + 1; y < p_end; y++) {
                    if (objects_differ(&by_sp[j]->rec.fact,
                                       &by_sp[y]->rec.fact)) {
                        found++;
                        note_conflict(db, by_sp[j], by_sp[y], ns,
                                      "cardinality: one");
                    }
                }
            }
            j = p_end;
        }

        /* mutex_with — two predicates the registry says cannot both hold.
         *
         * No short-circuit on the left-hand spec's own mutex list: the registry
         * validates that a mutex_with entry names a declared predicate, but —
         * unlike inverse_of — it does not require the declaration to be mutual.
         * Skipping on `!sx->mutex_with` therefore made a one-sided declaration
         * detectable or invisible depending on which predicate sorted first. */
        for (size_t x = i; x + 1 < s_end; x++) {
            const PredicateSpec *sx = predicate_registry_get(
                db->predicates, by_sp[x]->rec.fact.predicate);
            if (!sx) {
                continue;
            }
            for (size_t y = x + 1; y < s_end; y++) {
                const PredicateSpec *sy = predicate_registry_get(
                    db->predicates, by_sp[y]->rec.fact.predicate);
                if (sy && mutually_exclusive(sx, sy)) {
                    found++;
                    note_conflict(db, by_sp[x], by_sp[y], ns, "mutex_with");
                }
            }
        }
        i = s_end;
    }
    free(by_sp);
    return found;
}

/* One namespace's pass. Returns the number of conclusions written, and sets
 * *deferred when a cap stopped it short. */
static size_t run_namespace(AegisDB *db, const LoadedFact *facts, size_t n,
                            const char *ns, size_t budget, size_t *cand_budget,
                            int *deferred) {
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
    /* Shared across namespaces, not reset per group: a per-group cap would
     * make a tick's real ceiling groups × the cap, which on a server with many
     * tenants whose closures are already materialized is exactly the unbounded
     * work the candidate budget exists to prevent. */
    opts.max_candidates = *cand_budget;
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
    *cand_budget -= res.candidates_examined < *cand_budget
                        ? res.candidates_examined
                        : *cand_budget;
    if (res.truncated) {
        *deferred = 1;
        /* Advanced by facts, not candidates: start_index is a position in the
         * fact array, and one fact can yield as many candidates as it has join
         * partners. Advancing by the wrong unit skips facts wholesale, and a
         * candidate count that happens to be a multiple of the fact count
         * makes the rotation a no-op — the same prefix forever. */
        atomic_fetch_add_explicit(&db->infer_cursor,
                                  (uint_fast64_t)res.facts_scanned,
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

    uint64_t t0 = db_now_ms();
    size_t total = 0;
    size_t conflicts = 0;
    int deferred = 0;
    int scanned = 0; /* did this pass actually look? */
    LoadedFact *facts = NULL;
    size_t n = 0;
    if (load_facts(db, &facts, &n) != 0) {
        LOG_WARN("inference: could not read the fact set; skipping this pass");
        goto done;
    }
    if (n == 0) {
        goto done;
    }

    /* Namespace boundaries, so no rule ever joins two tenants' facts. */
    size_t ngroups = 0;
    for (size_t i = 0; i < n;) {
        ngroups++;
        size_t j = i;
        while (j < n && strcmp(facts[j].ns, facts[i].ns) == 0) {
            j++;
        }
        i = j;
    }
    /* Counted first and allocated to fit. A fixed array silently stopped
     * scanning past its bound, so tenants beyond it were never examined at
     * all — invisible, and worst on exactly the servers with most tenants. */
    size_t *starts = malloc(ngroups * sizeof(*starts));
    if (!starts) {
        LOG_WARN("inference: out of memory grouping namespaces; skipping");
        goto done;
    }
    {
        size_t g = 0;
        for (size_t i = 0; i < n;) {
            starts[g++] = i;
            size_t j = i;
            while (j < n && strcmp(facts[j].ns, facts[i].ns) == 0) {
                j++;
            }
            i = j;
        }
    }

    /* Contradictions first, and for *every* namespace regardless of the derive
     * budget. The gauge answers "does the corpus hold a contradiction right
     * now?", so a partial answer is worse than none: under budget pressure it
     * would report a different subset of tenants each tick and could read 0
     * while contradictions exist. Detection is linear per subject and only
     * writes when it finds something new, so completeness is affordable here in
     * a way deriving is not. */
    for (size_t g = 0; g < ngroups; g++) {
        size_t lo = starts[g];
        size_t hi = (g + 1 < ngroups) ? starts[g + 1] : n;
        conflicts += find_conflicts(db, &facts[lo], hi - lo, facts[lo].ns);
    }
    scanned = 1;

    size_t derived_budget = db->config.inference_max_derived;
    size_t cand_budget = db->config.inference_max_candidates;
    /* Groups are visited round-robin from a rotating offset. Always starting
     * at the first would let one busy tenant consume the whole write budget
     * every tick and starve every tenant that sorts after it — the budget is
     * shared, and strcmp order is not a fairness policy. */
    size_t g0 = ngroups ? (size_t)atomic_load_explicit(&db->infer_ns_cursor,
                                                       memory_order_relaxed) %
                              ngroups
                        : 0;
    size_t advanced = 0;
    for (size_t k = 0; k < ngroups; k++) {
        size_t g = (g0 + k) % ngroups;
        size_t lo = starts[g];
        size_t hi = (g + 1 < ngroups) ? starts[g + 1] : n;
        if (derived_budget == 0 || cand_budget == 0) {
            deferred = 1; /* the groups not reached wait for the next tick */
            break;
        }
        size_t got = run_namespace(db, &facts[lo], hi - lo, facts[lo].ns,
                                   derived_budget, &cand_budget, &deferred);
        derived_budget -= got < derived_budget ? got : derived_budget;
        total += got;
        advanced = k + 1;
    }
    /* Resume at the group after the last one reached, so the next tick starts
     * where this one ran out rather than repeating the same prefix. */
    if (deferred && ngroups) {
        atomic_store_explicit(&db->infer_ns_cursor,
                              (uint_fast64_t)((g0 + advanced) % ngroups),
                              memory_order_relaxed);
    }
    free(starts);

done:
    free_loaded(facts, n);
    /* Stored on every path, including the ones that did nothing: an operator
     * is told to alert on inference_deferred staying set, so leaving a stale 1
     * behind after the work drained away would be a permanent false alarm. */
    atomic_store_explicit(&db->infer_last_ms, db_now_ms() - t0,
                          memory_order_relaxed);
    atomic_store_explicit(&db->infer_deferred, deferred ? 1 : 0,
                          memory_order_relaxed);
    /* A gauge, not a running total: recomputed from scratch every pass, so it
     * reports how many contradictions the corpus holds *now*. A cumulative
     * counter would climb on every tick that re-found the same pair, which is
     * a worse number to alert on than the one that answers "is anything
     * contradictory right now?".
     *
     * Only published by a pass that actually scanned. Storing 0 after a failed
     * read would be a false all-clear, and an operator alerting on this would
     * watch the alarm silently clear itself on a transient error. */
    if (scanned) {
        atomic_store_explicit(&db->conflicts_now, (uint_fast64_t)conflicts,
                              memory_order_relaxed);
    }
    if (total) {
        atomic_fetch_add_explicit(&db->derived_total, (uint_fast64_t)total,
                                  memory_order_relaxed);
        LOG_DEBUG("inference: wrote %zu conclusion(s)%s", total,
                  deferred ? " (a cap deferred the rest)" : "");
    }
    return total;
}

/* ----- truth maintenance (ROADMAP 5.3 §6) -------------------------------- */

/* Push one id, growing the queue. Caller holds retract.lock. */
static void retract_push_locked(AegisDB *db, uint64_t id) {
    if (db->retract.n == db->retract.cap) {
        size_t nc = db->retract.cap ? db->retract.cap * 2 : 16;
        uint64_t *ni = realloc(db->retract.ids, nc * sizeof(*ni));
        if (!ni) {
            /* Recovery reconciles the live set — but only at process start, so
             * on a server that survives this the conclusion stays live until
             * then. Rare enough to tolerate, not rare enough to hide. */
            LOG_WARN("inference: retraction queue full; record %llu will not "
                     "be re-judged until the next restart",
                     (unsigned long long)id);
            return;
        }
        db->retract.ids = ni;
        db->retract.cap = nc;
    }
    db->retract.ids[db->retract.n++] = id;
}

void db_retract_enqueue_id(AegisDB *db, uint64_t id) {
    pthread_mutex_lock(&db->retract.lock);
    retract_push_locked(db, id);
    pthread_mutex_unlock(&db->retract.lock);
}

void db_retract_enqueue(AegisDB *db, uint64_t id) {
    if (!db->config.inference || !db->edges) {
        return; /* nothing here derives, so nothing here can be orphaned */
    }
    static const char *const KIND[] = {"derived_from"};
    EdgeSource *src = NULL;
    size_t n = 0;
    /* Caller holds index_lock for write, so the reverse index is stable and
     * still intact — this must run before edge_index_remove_target. */
    if (edge_index_sources(db->edges, id, KIND, 1, &src, &n) != 0 || n == 0) {
        free(src);
        return;
    }
    pthread_mutex_lock(&db->retract.lock);
    for (size_t i = 0; i < n; i++) {
        /* An over-long kind interns as unknown, which makes a filtered source
         * a candidate rather than a match (see edge_index.h). A candidate is
         * exactly what this queue wants: the drain re-reads the record and
         * decides from its routes, not from the edge label. */
        retract_push_locked(db, src[i].from_id);
    }
    pthread_mutex_unlock(&db->retract.lock);
    free(src);
}

/* Is this id a live record? */
int db_id_is_live(AegisDB *db, uint64_t id) {
    uint64_t now = db_now_ms();
    pthread_rwlock_rdlock(&db->index_lock);
    const HashEntry *e = hash_index_get(db->hash, id);
    /* Expiry counts as gone, not merely as pending. `get` and `search` already
     * treat an expired-but-unswept record as absent, so judging a premise live
     * until the sweep gets to it would leave a conclusion standing on something
     * no reader can see — and the sweep runs on its own much slower cadence,
     * so that window is nothing like the one tick the docs describe. */
    int live = e && !e->deleted && !(e->expires_at && now >= e->expires_at);
    pthread_rwlock_unlock(&db->index_lock);
    if (live) {
        return 1;
    }
    /* Merged is not lost. Consolidation tombstones a near-duplicate after
     * migrating what it held onto a survivor, so the fact that justified the
     * conclusion still exists under a different id. Retracting here would
     * withdraw something the corpus still supports, and the next pass would
     * re-derive it under a new id — churn visible in `history` and in the log.
     * The premise ids are deliberately *not* rewritten: a derivation should
     * record what it was actually drawn from, and the supersession chain is
     * what explains the rest. */
    uint64_t heir = db_supersede_resolve(db, id);
    if (!heir) {
        return 0;
    }
    uint64_t hnow = db_now_ms();
    pthread_rwlock_rdlock(&db->index_lock);
    const HashEntry *he = hash_index_get(db->hash, heir);
    int heir_live =
        he && !he->deleted && !(he->expires_at && hnow >= he->expires_at);
    pthread_rwlock_unlock(&db->index_lock);
    return heir_live;
}

/* Does any justification still stand? Support is disjunctive: one surviving
 * route is enough, and a conclusion with no routes is asserted, not derived,
 * so it is never this machinery's business. */
int db_derivation_stands(AegisDB *db, const MemoryRecord *r) {
    if (r->derivation.route_count == 0) {
        return 1;
    }
    for (size_t i = 0; i < r->derivation.route_count; i++) {
        const DerivRoute *rt = &r->derivation.routes[i];
        int all_live = 1;
        for (size_t j = 0; j < rt->premise_count && all_live; j++) {
            all_live = db_id_is_live(db, rt->premises[j]);
        }
        if (all_live) {
            return 1;
        }
    }
    return 0;
}

size_t db_retract_step(AegisDB *db) {
    /* A follower applies its primary's tombstones through the stream; deciding
     * locally would append frames the primary never sent. */
    if (db->config.read_only) {
        return 0;
    }
    /* Bounded like every other job on this tick. A forget-by-namespace or a
     * large expiry sweep tombstones thousands of premises at once, and each
     * dependent costs an index write lock, a log append and a flush — draining
     * the lot in one tick would stall the interval fsync, the checkpoint and
     * compaction behind it, and hold index_lock against client traffic. The
     * remainder stays queued for the next tick; a cascade was already going to
     * take several. */
    size_t cap = db->config.inference_max_derived;
    pthread_mutex_lock(&db->retract.lock);
    size_t n = db->retract.n < cap ? db->retract.n : cap;
    uint64_t *batch = NULL;
    if (n) {
        batch = malloc(n * sizeof(*batch));
        if (batch) {
            memcpy(batch, db->retract.ids, n * sizeof(*batch));
            db->retract.n -= n;
            memmove(db->retract.ids, db->retract.ids + n,
                    db->retract.n * sizeof(*batch));
        } else {
            n = 0;
        }
    }
    pthread_mutex_unlock(&db->retract.lock);
    if (n == 0) {
        return 0;
    }

    /* Drained to a local batch first: qe_delete below takes index_lock and
     * re-enters db_retract_enqueue for the next generation of dependents, so
     * holding the queue lock across it would deadlock. This is also what makes
     * the cascade breadth-first — each generation lands on a later tick. */
    size_t retracted = 0;
    for (size_t i = 0; i < n; i++) {
        MemoryRecord r;
        if (qe_get(db, batch[i], NULL, &r) != AEGIS_OK) {
            continue; /* already gone */
        }
        int stands = db->config.inference ? db_derivation_stands(db, &r) : 1;
        uint64_t id = r.id;
        record_free(&r);
        if (stands) {
            continue;
        }
        if (qe_delete(db, id, NULL) == AEGIS_OK) {
            retracted++;
            LOG_DEBUG("inference: retracted record %llu; no route survives",
                      (unsigned long long)id);
        }
    }
    free(batch);
    if (retracted) {
        atomic_fetch_add_explicit(&db->retracted_total,
                                  (uint_fast64_t)retracted,
                                  memory_order_relaxed);
    }
    return retracted;
}

/* ----- supersession, so a merge is not mistaken for a loss ---------------- */

/* Beyond this a merge history is pathological, not interesting.
 *
 * Chains only reach this length *within* one process run. Recovery rebuilds the
 * mapping from live records' own `supersedes` edges, and a middle link is
 * itself tombstoned — its edges are on a record the live-set walk never visits
 * — so after a restart a twice-merged premise resolves to nothing and reads as
 * lost. That costs a retraction and a re-derivation, which is the same price
 * the cap charges, so it is a documented limit rather than a bug to chase. */
#define SUPERSEDE_MAX_HOPS 8
/* Entries are tiny and only consolidation creates them, but a long-lived
 * server merging continuously should not grow this without bound. Past the cap
 * a merged premise reads as lost, which costs a retraction and a
 * re-derivation — churn, not a wrong answer. */
#define SUPERSEDE_MAX 65536

void db_supersede_note(AegisDB *db, uint64_t old_id, uint64_t new_id) {
    if (!db->config.inference || old_id == new_id) {
        return;
    }
    pthread_mutex_lock(&db->retract.lock);
    if (db->retract.sup_n >= SUPERSEDE_MAX) {
        pthread_mutex_unlock(&db->retract.lock);
        return;
    }
    if (db->retract.sup_n == db->retract.sup_cap) {
        size_t nc = db->retract.sup_cap ? db->retract.sup_cap * 2 : 32;
        uint64_t *nf = realloc(db->retract.sup_from, nc * sizeof(*nf));
        if (!nf) {
            pthread_mutex_unlock(&db->retract.lock);
            return;
        }
        /* Adopted before the second realloc is attempted. A successful realloc
         * may move the block and free the old one, so leaving the stale pointer
         * in the struct while the second call fails would strand a freed
         * pointer there — realloc'd again on the next grow, and freed a second
         * time by db_close. */
        db->retract.sup_from = nf;
        uint64_t *nt = realloc(db->retract.sup_to, nc * sizeof(*nt));
        if (!nt) {
            pthread_mutex_unlock(&db->retract.lock);
            return; /* sup_cap unchanged, so the two arrays stay consistent */
        }
        db->retract.sup_to = nt;
        db->retract.sup_cap = nc;
    }
    db->retract.sup_from[db->retract.sup_n] = old_id;
    db->retract.sup_to[db->retract.sup_n] = new_id;
    db->retract.sup_n++;
    atomic_store_explicit(&db->retract.sup_live, 1, memory_order_relaxed);
    pthread_mutex_unlock(&db->retract.lock);
}

/* One hop, under the lock. */
static uint64_t supersede_lookup(AegisDB *db, uint64_t id) {
    uint64_t to = 0;
    /* The common case is an empty map, and this lock is taken by qe_delete
     * while it holds index_lock for write — so scanning here parks every
     * reader behind it. Entries are only ever appended, so a relaxed read of
     * this flag is enough to skip the lock entirely when there is nothing to
     * find. */
    if (atomic_load_explicit(&db->retract.sup_live, memory_order_relaxed) ==
        0) {
        return 0;
    }
    pthread_mutex_lock(&db->retract.lock);
    for (size_t i = db->retract.sup_n; i-- > 0;) {
        if (db->retract.sup_from[i] == id) {
            to = db->retract.sup_to[i];
            break; /* newest wins: the same id can be merged onward again */
        }
    }
    pthread_mutex_unlock(&db->retract.lock);
    return to;
}

uint64_t db_supersede_resolve(AegisDB *db, uint64_t id) {
    uint64_t cur = id;
    for (int hop = 0; hop < SUPERSEDE_MAX_HOPS; hop++) {
        uint64_t next = supersede_lookup(db, cur);
        if (!next || next == cur) {
            return hop ? cur : 0;
        }
        cur = next;
    }
    return cur;
}
