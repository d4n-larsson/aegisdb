/* Unit tests for the pure inference closures — ROADMAP 5.3, PR 2.
 *
 * The module has no database, so everything it can get wrong is reachable from
 * here: which conclusions are drawn, which are suppressed as duplicates, what
 * happens in a cycle, how depth and confidence propagate, and what the caps do.
 * Those are the parts that fail quietly once a background job is driving it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/inference.h"
#include "unity.h"

static char g_path[256];

void setUp(void) {
    snprintf(g_path, sizeof(g_path), "/tmp/aegis_infer_%d_%ld.json",
             (int)getpid(), (long)random());
}

void tearDown(void) { unlink(g_path); }

static PredicateRegistry *load(const char *json) {
    FILE *fh = fopen(g_path, "wb");
    TEST_ASSERT_NOT_NULL(fh);
    fwrite(json, 1, strlen(json), fh);
    fclose(fh);
    char err[256] = "";
    PredicateRegistry *r = predicate_registry_load(g_path, err, sizeof err);
    if (!r) {
        TEST_FAIL_MESSAGE(err);
    }
    return r;
}

/* An id-object fact asserted by `rid` at depth 0, confidence 1. */
static InferFact f_id(uint64_t rid, uint64_t s, const char *p, uint64_t o) {
    InferFact f = {0};
    f.record_id = rid;
    f.subject = s;
    f.predicate = p;
    f.object_kind = FACT_OBJ_ID;
    f.object_id = o;
    f.confidence = 1.0F;
    return f;
}

/* Is (s, p, o) among the conclusions, and if so which one? */
static const InferConclusion *find(const InferResult *r, uint64_t s,
                                   const char *p, uint64_t o) {
    for (size_t i = 0; i < r->n; i++) {
        const InferConclusion *c = &r->items[i];
        if (c->subject == s && c->object_kind == FACT_OBJ_ID &&
            c->object_id == o && strcmp(c->predicate, p) == 0) {
            return c;
        }
    }
    return NULL;
}

/* ----- the three rules --------------------------------------------------- */

static void test_transitive(void) {
    PredicateRegistry *reg =
        load("{\"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    InferFact facts[] = {
        f_id(1, 10, "part_of", 20),
        f_id(2, 20, "part_of", 30),
    };
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 2, reg, NULL, &res));
    TEST_ASSERT_EQUAL_size_t(1, res.n);

    const InferConclusion *c = find(&res, 10, "part_of", 30);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(DERIV_TRANSITIVE, c->routes[0].rule);
    TEST_ASSERT_EQUAL_size_t(2, c->routes[0].premise_count);
    TEST_ASSERT_EQUAL_UINT64(1, c->routes[0].premises[0]);
    TEST_ASSERT_EQUAL_UINT64(2, c->routes[0].premises[1]);
    TEST_ASSERT_EQUAL_UINT16(1, c->routes[0].depth);
    TEST_ASSERT_EQUAL_INT(0, res.truncated);

    infer_result_free(&res);
    predicate_registry_free(reg);
}

static void test_symmetric(void) {
    PredicateRegistry *reg =
        load("{\"conflicts_with\": {\"object\": \"id\", \"symmetric\": true}}");
    InferFact facts[] = {f_id(1, 10, "conflicts_with", 20)};
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 1, reg, NULL, &res));
    TEST_ASSERT_EQUAL_size_t(1, res.n);

    const InferConclusion *c = find(&res, 20, "conflicts_with", 10);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(DERIV_SYMMETRIC, c->routes[0].rule);
    TEST_ASSERT_EQUAL_size_t(1, c->routes[0].premise_count);
    TEST_ASSERT_EQUAL_UINT64(1, c->routes[0].premises[0]);

    infer_result_free(&res);
    predicate_registry_free(reg);
}

static void test_inverse(void) {
    PredicateRegistry *reg = load(
        "{\"part_of\": {\"object\": \"id\", \"inverse_of\": \"contains\"},"
        " \"contains\": {\"object\": \"id\", \"inverse_of\": \"part_of\"}}");
    InferFact facts[] = {f_id(1, 10, "part_of", 20)};
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 1, reg, NULL, &res));
    TEST_ASSERT_EQUAL_size_t(1, res.n);

    const InferConclusion *c = find(&res, 20, "contains", 10);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(DERIV_INVERSE, c->routes[0].rule);

    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* ----- what must NOT be concluded ---------------------------------------- */

/* Nothing is derivable without a vocabulary that declares it — the default
 * server configuration, and the reason inference is invisible when unused. */
static void test_no_registry_concludes_nothing(void) {
    InferFact facts[] = {f_id(1, 10, "part_of", 20),
                         f_id(2, 20, "part_of", 30)};
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 2, NULL, NULL, &res));
    TEST_ASSERT_EQUAL_size_t(0, res.n);
    infer_result_free(&res);
}

/* A predicate the registry does not declare, and one declared with no
 * relational property, both conclude nothing. */
static void test_undeclared_and_inert_predicates(void) {
    PredicateRegistry *reg = load("{\"knows\": {\"object\": \"id\"}}");
    InferFact facts[] = {
        f_id(1, 10, "knows", 20),   /* declared, but inert */
        f_id(2, 20, "knows", 30),   /* ditto */
        f_id(3, 10, "unheard", 20), /* not declared at all */
    };
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 3, reg, NULL, &res));
    TEST_ASSERT_EQUAL_size_t(0, res.n);
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* A literal object cannot be a subject, so it never participates — but it must
 * still be counted for dedup, which the next test covers. */
static void test_literal_objects_are_skipped(void) {
    PredicateRegistry *reg =
        load("{\"defaults_to\": {\"object\": \"string\"},"
             " \"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    InferFact lit = {0};
    lit.record_id = 1;
    lit.subject = 10;
    lit.predicate = "defaults_to";
    lit.object_kind = FACT_OBJ_STRING;
    lit.object_str = "none";
    lit.confidence = 1.0F;
    InferFact facts[] = {lit, f_id(2, 10, "part_of", 20)};
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 2, reg, NULL, &res));
    TEST_ASSERT_EQUAL_size_t(0, res.n);
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* THE idempotence test. A conclusion already present in the snapshot is not
 * drawn again — which is what makes re-running the job over a quiet corpus
 * cost index probes and zero writes. */
static void test_existing_conclusion_is_not_redrawn(void) {
    PredicateRegistry *reg =
        load("{\"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    InferFact facts[] = {
        f_id(1, 10, "part_of", 20),
        f_id(2, 20, "part_of", 30),
        f_id(3, 10, "part_of", 30), /* the conclusion, already a fact */
    };
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 3, reg, NULL, &res));
    TEST_ASSERT_EQUAL_size_t(0, res.n);
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* Two routes to the same triple produce it once, not twice. */
static void test_duplicate_conclusions_collapse(void) {
    PredicateRegistry *reg =
        load("{\"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    /* 10->20->40 and 10->30->40 both conclude (10 part_of 40) */
    InferFact facts[] = {
        f_id(1, 10, "part_of", 20),
        f_id(2, 20, "part_of", 40),
        f_id(3, 10, "part_of", 30),
        f_id(4, 30, "part_of", 40),
    };
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 4, reg, NULL, &res));
    size_t hits = 0;
    for (size_t i = 0; i < res.n; i++) {
        if (res.items[i].subject == 10 && res.items[i].object_id == 40) {
            hits++;
        }
    }
    TEST_ASSERT_EQUAL_size_t(1, hits); /* one conclusion... */
    const InferConclusion *both = find(&res, 10, "part_of", 40);
    TEST_ASSERT_NOT_NULL(both);
    TEST_ASSERT_EQUAL_size_t(2, both->route_count); /* ...justified two ways */
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* ----- cycles ------------------------------------------------------------ */

/* A 2-cycle terminates without a cycle detector: it concludes each self-fact
 * once, and a second pass over the resulting set concludes nothing at all. */
static void test_cycle_terminates_via_dedup(void) {
    PredicateRegistry *reg =
        load("{\"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    InferFact facts[] = {
        f_id(1, 10, "part_of", 20),
        f_id(2, 20, "part_of", 10),
    };
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 2, reg, NULL, &res));
    TEST_ASSERT_NOT_NULL(find(&res, 10, "part_of", 10));
    TEST_ASSERT_NOT_NULL(find(&res, 20, "part_of", 20));
    TEST_ASSERT_EQUAL_size_t(2, res.n);

    /* Feed the conclusions back in as facts, as the next pass would see them:
     * the pass is now dry. */
    InferFact round2[4];
    round2[0] = facts[0];
    round2[1] = facts[1];
    round2[2] = f_id(3, 10, "part_of", 10);
    round2[3] = f_id(4, 20, "part_of", 20);
    InferResult res2;
    TEST_ASSERT_EQUAL_INT(0, infer_run(round2, 4, reg, NULL, &res2));
    TEST_ASSERT_EQUAL_size_t(0, res2.n);

    infer_result_free(&res2);
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* A self-fact concludes itself under every rule, and every one is a duplicate
 * of the input. */
static void test_self_fact_concludes_nothing_new(void) {
    PredicateRegistry *reg =
        load("{\"peer\": {\"object\": \"id\", \"symmetric\": true, "
             "\"transitive\": true}}");
    InferFact facts[] = {f_id(1, 10, "peer", 10)};
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 1, reg, NULL, &res));
    TEST_ASSERT_EQUAL_size_t(0, res.n);
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* ----- attribution ------------------------------------------------------- */

/* Depth is one past the deepest premise, and confidence is the product. Above
 * the floor that makes a conclusion no more confident than its premises; the
 * floor itself can raise it, which the next test covers. */
static void test_depth_and_confidence_propagate(void) {
    PredicateRegistry *reg =
        load("{\"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    InferFact facts[] = {
        f_id(1, 10, "part_of", 20),
        f_id(2, 20, "part_of", 30),
    };
    facts[0].depth = 2;
    facts[0].confidence = 0.5F;
    facts[1].depth = 1;
    facts[1].confidence = 0.4F;

    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 2, reg, NULL, &res));
    const InferConclusion *c = find(&res, 10, "part_of", 30);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT16(3, c->routes[0].depth); /* max(2,1) + 1 */
    TEST_ASSERT_FLOAT_WITHIN(1e-6F, 0.2F, c->confidence);
    TEST_ASSERT_TRUE(c->confidence <= facts[0].confidence);
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* The floor keeps a long chain from decaying into noise — and in doing so it
 * deliberately breaks monotonicity, raising the conclusion above both premises.
 * Asserted explicitly so the trade is visible rather than surprising. */
static void test_confidence_floor(void) {
    PredicateRegistry *reg =
        load("{\"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    InferFact facts[] = {
        f_id(1, 10, "part_of", 20),
        f_id(2, 20, "part_of", 30),
    };
    facts[0].confidence = 0.01F;
    facts[1].confidence = 0.01F; /* product 0.0001, well under the floor */

    InferOpts opts = {0};
    opts.confidence_floor = 0.25F;
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 2, reg, &opts, &res));
    const InferConclusion *c = find(&res, 10, "part_of", 30);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FLOAT_WITHIN(1e-6F, 0.25F, c->confidence);
    TEST_ASSERT_TRUE(c->confidence > facts[0].confidence); /* not monotonic */
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* ----- caps -------------------------------------------------------------- */

static void test_max_depth_stops_the_chain(void) {
    PredicateRegistry *reg =
        load("{\"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    InferFact facts[] = {
        f_id(1, 10, "part_of", 20),
        f_id(2, 20, "part_of", 30),
    };
    facts[0].depth = 4; /* a conclusion would be depth 5 */

    InferOpts opts = {0};
    opts.max_depth = 4;
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 2, reg, &opts, &res));
    TEST_ASSERT_EQUAL_size_t(0, res.n);

    opts.max_depth = 5; /* one deeper: now it fits */
    InferResult res2;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 2, reg, &opts, &res2));
    TEST_ASSERT_EQUAL_size_t(1, res2.n);
    TEST_ASSERT_EQUAL_UINT16(5, res2.items[0].routes[0].depth);

    infer_result_free(&res2);
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* Hitting the conclusion cap is reported, not silent: a pass that never
 * reaches fixpoint is survivable, but only if it can be seen. */
static void test_max_conclusions_truncates_and_says_so(void) {
    PredicateRegistry *reg =
        load("{\"conflicts_with\": {\"object\": \"id\", \"symmetric\": true}}");
    InferFact facts[] = {
        f_id(1, 10, "conflicts_with", 20),
        f_id(2, 30, "conflicts_with", 40),
        f_id(3, 50, "conflicts_with", 60),
    };
    InferOpts opts = {0};
    opts.max_conclusions = 2;
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 3, reg, &opts, &res));
    TEST_ASSERT_EQUAL_size_t(2, res.n);
    TEST_ASSERT_EQUAL_INT(1, res.truncated);

    /* Uncapped, all three are drawn and nothing is reported truncated. */
    InferResult res2;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 3, reg, NULL, &res2));
    TEST_ASSERT_EQUAL_size_t(3, res2.n);
    TEST_ASSERT_EQUAL_INT(0, res2.truncated);

    infer_result_free(&res2);
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* ----- shape ------------------------------------------------------------- */

static void test_empty_input(void) {
    PredicateRegistry *reg =
        load("{\"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(NULL, 0, reg, NULL, &res));
    TEST_ASSERT_EQUAL_size_t(0, res.n);
    TEST_ASSERT_NULL(res.items);
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* One pass draws from the input only, so a 3-link chain needs two passes to
 * close. Documented behaviour, pinned here so it cannot drift into an
 * unbounded fixpoint loop by accident. */
static void test_closure_takes_successive_passes(void) {
    PredicateRegistry *reg =
        load("{\"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    InferFact facts[] = {
        f_id(1, 10, "part_of", 20),
        f_id(2, 20, "part_of", 30),
        f_id(3, 30, "part_of", 40),
    };
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 3, reg, NULL, &res));
    TEST_ASSERT_NOT_NULL(find(&res, 10, "part_of", 30));
    TEST_ASSERT_NOT_NULL(find(&res, 20, "part_of", 40));
    TEST_ASSERT_NULL(find(&res, 10, "part_of", 40)); /* not this pass */
    TEST_ASSERT_EQUAL_size_t(2, res.n);

    InferFact round2[5];
    memcpy(round2, facts, sizeof(facts));
    round2[3] = f_id(4, 10, "part_of", 30);
    round2[4] = f_id(5, 20, "part_of", 40);
    InferResult res2;
    TEST_ASSERT_EQUAL_INT(0, infer_run(round2, 5, reg, NULL, &res2));
    TEST_ASSERT_NOT_NULL(find(&res2, 10, "part_of", 40)); /* now it closes */

    infer_result_free(&res2);
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* The conclusion set does not depend on input order, which is what lets the
 * caller choose an order for reproducibility rather than for correctness. */
static void test_conclusion_set_is_order_independent(void) {
    PredicateRegistry *reg =
        load("{\"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    InferFact fwd[] = {
        f_id(1, 10, "part_of", 20),
        f_id(2, 20, "part_of", 30),
        f_id(3, 30, "part_of", 40),
    };
    InferFact rev[] = {fwd[2], fwd[1], fwd[0]};

    InferResult a;
    InferResult b;
    TEST_ASSERT_EQUAL_INT(0, infer_run(fwd, 3, reg, NULL, &a));
    TEST_ASSERT_EQUAL_INT(0, infer_run(rev, 3, reg, NULL, &b));
    TEST_ASSERT_EQUAL_size_t(a.n, b.n);
    for (size_t i = 0; i < a.n; i++) {
        TEST_ASSERT_NOT_NULL(find(&b, a.items[i].subject, a.items[i].predicate,
                                  a.items[i].object_id));
    }
    infer_result_free(&b);
    infer_result_free(&a);
    predicate_registry_free(reg);
}

/* ----- bounded work ------------------------------------------------------ */

/* A corpus whose closure is already materialized offers a candidate for every
 * pair and keeps none of them. Capping *conclusions* never fires there, so the
 * pass would grow with the corpus — cubically, for a chain. The candidate
 * budget is what actually bounds a tick, and this is the shape that proves it:
 * a fully closed chain, where every candidate is a duplicate. */
static void test_work_budget_bounds_a_closed_corpus(void) {
    PredicateRegistry *reg =
        load("{\"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    enum { N = 120 };
    static InferFact facts[N * (N - 1) / 2];
    size_t n = 0;
    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            facts[n] = f_id(n + 1, (uint64_t)i, "part_of", (uint64_t)j);
            n++;
        }
    }
    InferOpts opts = {0};
    opts.max_depth = 100;
    opts.max_conclusions = 1000; /* never reached: everything is a duplicate */
    opts.max_candidates = 5000;
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, n, reg, &opts, &res));
    TEST_ASSERT_EQUAL_size_t(0, res.n);      /* nothing new to conclude */
    TEST_ASSERT_EQUAL_INT(1, res.truncated); /* and it says it stopped early */
    TEST_ASSERT_TRUE(res.candidates_examined <= 5001);
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* A budgeted pass that always began at 0 would examine the same prefix forever.
 * start_index is what lets the caller sweep the rest. */
static void test_start_index_rotates_the_scan(void) {
    PredicateRegistry *reg =
        load("{\"conflicts_with\": {\"object\": \"id\", \"symmetric\": true}}");
    InferFact facts[] = {
        f_id(1, 10, "conflicts_with", 20),
        f_id(2, 30, "conflicts_with", 40),
        f_id(3, 50, "conflicts_with", 60),
    };
    InferOpts opts = {0};
    opts.max_candidates = 1;
    InferResult a;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 3, reg, &opts, &a));
    TEST_ASSERT_NOT_NULL(find(&a, 20, "conflicts_with", 10));

    opts.start_index = 2; /* the third fact, this time */
    InferResult b;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 3, reg, &opts, &b));
    TEST_ASSERT_NOT_NULL(find(&b, 60, "conflicts_with", 50));
    TEST_ASSERT_NULL(find(&b, 20, "conflicts_with", 10));

    infer_result_free(&b);
    infer_result_free(&a);
    predicate_registry_free(reg);
}

/* Meeting the conclusion cap and then seeing only duplicates is a *complete*
 * pass, not a deferred one — reporting otherwise would have the caller
 * scheduling follow-up work that does not exist. */
static void test_duplicates_past_the_cap_are_not_truncation(void) {
    PredicateRegistry *reg =
        load("{\"conflicts_with\": {\"object\": \"id\", \"symmetric\": true}}");
    /* 30<->40 is already symmetric in the input, so it yields no conclusion;
     * only 10->20 does. One conclusion exists in total. */
    InferFact facts[] = {
        f_id(1, 10, "conflicts_with", 20),
        f_id(2, 30, "conflicts_with", 40),
        f_id(3, 40, "conflicts_with", 30),
    };
    InferOpts opts = {0};
    opts.max_conclusions = 1;
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 3, reg, &opts, &res));
    TEST_ASSERT_EQUAL_size_t(1, res.n);
    TEST_ASSERT_EQUAL_INT(0, res.truncated);
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* ----- provenance -------------------------------------------------------- */

/* Two routes to the same triple must BOTH be recorded — support is
 * disjunctive, so keeping only one would let retraction drop a conclusion the
 * other route still justifies. And the set must not depend on which the scan
 * met first, because it ends up in a durable record. */
static void test_both_routes_are_recorded_and_ordered(void) {
    PredicateRegistry *reg =
        load("{\"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    InferFact fwd[] = {
        f_id(1, 10, "part_of", 20),
        f_id(2, 20, "part_of", 40),
        f_id(3, 10, "part_of", 30),
        f_id(4, 30, "part_of", 40),
    };
    InferFact rev[] = {fwd[2], fwd[3], fwd[0], fwd[1]};

    InferResult a;
    InferResult b;
    TEST_ASSERT_EQUAL_INT(0, infer_run(fwd, 4, reg, NULL, &a));
    TEST_ASSERT_EQUAL_INT(0, infer_run(rev, 4, reg, NULL, &b));
    const InferConclusion *ca = find(&a, 10, "part_of", 40);
    const InferConclusion *cb = find(&b, 10, "part_of", 40);
    TEST_ASSERT_NOT_NULL(ca);
    TEST_ASSERT_NOT_NULL(cb);
    /* both justifications survive, in the same order, whichever way we scanned */
    TEST_ASSERT_EQUAL_size_t(2, ca->route_count);
    TEST_ASSERT_EQUAL_size_t(2, cb->route_count);
    for (size_t i = 0; i < 2; i++) {
        TEST_ASSERT_EQUAL_UINT64(ca->routes[i].premises[0],
                                 cb->routes[i].premises[0]);
        TEST_ASSERT_EQUAL_UINT64(ca->routes[i].premises[1],
                                 cb->routes[i].premises[1]);
    }
    TEST_ASSERT_EQUAL_UINT64(1, ca->routes[0].premises[0]); /* 10->20->40 */
    TEST_ASSERT_EQUAL_UINT64(2, ca->routes[0].premises[1]);
    TEST_ASSERT_EQUAL_UINT64(3, ca->routes[1].premises[0]); /* 10->30->40 */
    TEST_ASSERT_EQUAL_UINT64(4, ca->routes[1].premises[1]);
    infer_result_free(&b);
    infer_result_free(&a);
    predicate_registry_free(reg);
}

/* A premise at the top of the depth range must not wrap to 0: a conclusion at
 * depth 0 is indistinguishable from an asserted fact, and the chain cap would
 * never bite again. */
static void test_depth_does_not_wrap(void) {
    PredicateRegistry *reg =
        load("{\"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    InferFact facts[] = {
        f_id(1, 10, "part_of", 20),
        f_id(2, 20, "part_of", 30),
    };
    facts[0].depth = UINT16_MAX;
    InferOpts opts = {0};
    opts.max_depth = UINT16_MAX;
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, 2, reg, &opts, &res));
    TEST_ASSERT_EQUAL_size_t(0, res.n); /* refused, not wrapped to depth 0 */
    infer_result_free(&res);
    predicate_registry_free(reg);
}

/* Past DERIV_MAX_ROUTES the lowest-ordered routes are kept. Dropping a route
 * can only cost a retraction-and-re-derivation, never a wrong answer, so the
 * cap is a memory bound rather than a correctness one. */
static void test_routes_are_capped_keeping_the_lowest(void) {
    PredicateRegistry *reg =
        load("{\"part_of\": {\"object\": \"id\", \"transitive\": true}}");
    /* six intermediates, so (10 part_of 99) has six justifications */
    InferFact facts[13];
    size_t n = 0;
    for (uint64_t m = 0; m < 6; m++) {
        facts[n] = f_id(n + 1, 10, "part_of", 20 + m);
        n++;
        facts[n] = f_id(n + 1, 20 + m, "part_of", 99);
        n++;
    }
    InferResult res;
    TEST_ASSERT_EQUAL_INT(0, infer_run(facts, n, reg, NULL, &res));
    const InferConclusion *c = find(&res, 10, "part_of", 99);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_size_t(DERIV_MAX_ROUTES, c->route_count);
    /* kept in premise-id order, lowest first */
    for (size_t i = 1; i < c->route_count; i++) {
        TEST_ASSERT_TRUE(c->routes[i - 1].premises[0] <
                         c->routes[i].premises[0]);
    }
    TEST_ASSERT_EQUAL_UINT64(1, c->routes[0].premises[0]);
    infer_result_free(&res);
    predicate_registry_free(reg);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_transitive);
    RUN_TEST(test_symmetric);
    RUN_TEST(test_inverse);
    RUN_TEST(test_no_registry_concludes_nothing);
    RUN_TEST(test_undeclared_and_inert_predicates);
    RUN_TEST(test_literal_objects_are_skipped);
    RUN_TEST(test_existing_conclusion_is_not_redrawn);
    RUN_TEST(test_duplicate_conclusions_collapse);
    RUN_TEST(test_cycle_terminates_via_dedup);
    RUN_TEST(test_self_fact_concludes_nothing_new);
    RUN_TEST(test_depth_and_confidence_propagate);
    RUN_TEST(test_confidence_floor);
    RUN_TEST(test_max_depth_stops_the_chain);
    RUN_TEST(test_max_conclusions_truncates_and_says_so);
    RUN_TEST(test_empty_input);
    RUN_TEST(test_closure_takes_successive_passes);
    RUN_TEST(test_conclusion_set_is_order_independent);
    RUN_TEST(test_work_budget_bounds_a_closed_corpus);
    RUN_TEST(test_start_index_rotates_the_scan);
    RUN_TEST(test_duplicates_past_the_cap_are_not_truncation);
    RUN_TEST(test_both_routes_are_recorded_and_ordered);
    RUN_TEST(test_depth_does_not_wrap);
    RUN_TEST(test_routes_are_capped_keeping_the_lowest);
    return UNITY_END();
}
