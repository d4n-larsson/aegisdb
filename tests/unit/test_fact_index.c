/* Unit tests for the fact indexes — ROADMAP 5.2.
 *
 * Three tables have to agree: a fact reachable by its subject but not by its
 * object would answer some patterns and silently not others, which is the
 * failure mode most worth pinning down here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aegisdb/fact_index.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- helpers ------------------------------------------------------------ */

static int has(const uint64_t *v, size_t n, uint64_t id) {
    for (size_t i = 0; i < n; i++) {
        if (v[i] == id) {
            return 1;
        }
    }
    return 0;
}

static void assert_sorted_unique(const uint64_t *v, size_t n) {
    for (size_t i = 1; i < n; i++) {
        TEST_ASSERT_TRUE(v[i - 1] < v[i]);
    }
}

/* Index a string-object fact: record `rec` asserts (subject, pred, "obj"). */
static int add_str(FactIndex *f, uint64_t rec, uint64_t subj, const char *pred,
                   const char *obj) {
    return fact_index_add(f, rec, subj, pred, FACT_OBJ_STRING, 0, obj);
}

static int add_id(FactIndex *f, uint64_t rec, uint64_t subj, const char *pred,
                  uint64_t obj) {
    return fact_index_add(f, rec, subj, pred, FACT_OBJ_ID, obj, NULL);
}

/* ---- the three lookups ------------------------------------------------- */

static void test_all_three_lookups_find_the_fact(void) {
    FactIndex *f = fact_index_create();
    TEST_ASSERT_NOT_NULL(f);
    /* record 100: subject 42 defaults_to "none" */
    TEST_ASSERT_EQUAL_INT(0, add_str(f, 100, 42, "defaults_to", "none"));

    uint64_t *out = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(
        0, fact_index_by_subject(f, 42, "defaults_to", &out, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(100, out[0]);
    free(out);

    TEST_ASSERT_EQUAL_INT(0, fact_index_by_object(f, FACT_OBJ_STRING, 0, "none",
                                                  "defaults_to", &out, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(100, out[0]);
    free(out);

    TEST_ASSERT_EQUAL_INT(0,
                          fact_index_by_predicate(f, "defaults_to", &out, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(100, out[0]);
    free(out);

    TEST_ASSERT_EQUAL_size_t(1, fact_index_facts(f));
    TEST_ASSERT_EQUAL_size_t(1, fact_index_predicates(f));
    fact_index_free(f);
}

/* A NULL predicate is the wildcard: everything about a subject, or everything
 * pointing at an object. */
static void test_wildcard_predicate(void) {
    FactIndex *f = fact_index_create();
    add_str(f, 100, 42, "defaults_to", "none");
    add_str(f, 101, 42, "described_as", "a hook");
    add_str(f, 102, 43, "defaults_to", "none");

    uint64_t *out = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, fact_index_by_subject(f, 42, NULL, &out, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    assert_sorted_unique(out, n);
    TEST_ASSERT_TRUE(has(out, n, 100) && has(out, n, 101));
    free(out);

    /* narrowed by predicate again */
    TEST_ASSERT_EQUAL_INT(
        0, fact_index_by_subject(f, 42, "defaults_to", &out, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(100, out[0]);
    free(out);

    /* both subjects assert "none", so the object side sees both records */
    TEST_ASSERT_EQUAL_INT(
        0, fact_index_by_object(f, FACT_OBJ_STRING, 0, "none", NULL, &out, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_TRUE(has(out, n, 100) && has(out, n, 102));
    free(out);
    fact_index_free(f);
}

/* An id object and a string object live in the same table; a string whose hash
 * happened to equal an id must not be confused with it. */
static void test_id_and_string_objects_do_not_collide(void) {
    FactIndex *f = fact_index_create();
    TEST_ASSERT_EQUAL_INT(0, add_id(f, 200, 1, "part_of", 7));
    TEST_ASSERT_EQUAL_INT(0, add_str(f, 201, 1, "part_of", "7"));

    uint64_t *out = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(
        0, fact_index_by_object(f, FACT_OBJ_ID, 7, NULL, "part_of", &out, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(200, out[0]);
    free(out);

    TEST_ASSERT_EQUAL_INT(0, fact_index_by_object(f, FACT_OBJ_STRING, 0, "7",
                                                  "part_of", &out, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(201, out[0]);
    free(out);

    /* the subject side sees both, since both are facts about subject 1 */
    TEST_ASSERT_EQUAL_INT(0, fact_index_by_subject(f, 1, "part_of", &out, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    free(out);
    fact_index_free(f);
}

static void test_distinct_string_objects_stay_distinct(void) {
    FactIndex *f = fact_index_create();
    add_str(f, 1, 1, "p", "alpha");
    add_str(f, 2, 1, "p", "beta");
    uint64_t *out = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(
        0, fact_index_by_object(f, FACT_OBJ_STRING, 0, "alpha", "p", &out, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(1, out[0]);
    free(out);
    /* an object nobody asserted */
    TEST_ASSERT_EQUAL_INT(
        0, fact_index_by_object(f, FACT_OBJ_STRING, 0, "gamma", "p", &out, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_NULL(out);
    fact_index_free(f);
}

/* Two records asserting the same triple both surface, once each. */
static void test_same_triple_from_two_records(void) {
    FactIndex *f = fact_index_create();
    add_str(f, 10, 1, "p", "o");
    add_str(f, 11, 1, "p", "o");
    uint64_t *out = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, fact_index_by_subject(f, 1, "p", &out, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    assert_sorted_unique(out, n);
    free(out);
    TEST_ASSERT_EQUAL_size_t(2, fact_index_facts(f));
    TEST_ASSERT_EQUAL_size_t(1, fact_index_predicates(f));
    fact_index_free(f);
}

static void test_add_is_idempotent(void) {
    FactIndex *f = fact_index_create();
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_INT(0, add_str(f, 10, 1, "p", "o"));
    }
    TEST_ASSERT_EQUAL_size_t(1, fact_index_facts(f));
    uint64_t *out = NULL;
    size_t n = 0;
    fact_index_by_subject(f, 1, "p", &out, &n);
    TEST_ASSERT_EQUAL_size_t(1, n);
    free(out);
    fact_index_free(f);
}

/* ---- removal ----------------------------------------------------------- */

static void test_remove_clears_all_three_tables(void) {
    FactIndex *f = fact_index_create();
    add_str(f, 10, 1, "p", "o");
    add_str(f, 11, 2, "p", "o"); /* a bystander sharing predicate and object */

    fact_index_remove(f, 10, 1, "p", FACT_OBJ_STRING, 0, "o");
    TEST_ASSERT_EQUAL_size_t(1, fact_index_facts(f));

    uint64_t *out = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, fact_index_by_subject(f, 1, "p", &out, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_NULL(out);
    TEST_ASSERT_EQUAL_INT(
        0, fact_index_by_object(f, FACT_OBJ_STRING, 0, "o", "p", &out, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(11, out[0]); /* only the bystander remains */
    free(out);
    TEST_ASSERT_EQUAL_INT(0, fact_index_by_predicate(f, "p", &out, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(11, out[0]);
    free(out);

    /* removing what is not there is a no-op, not a corruption */
    fact_index_remove(f, 999, 1, "p", FACT_OBJ_STRING, 0, "o");
    fact_index_remove(f, 11, 2, "nope", FACT_OBJ_STRING, 0, "o");
    TEST_ASSERT_EQUAL_size_t(1, fact_index_facts(f));
    fact_index_free(f);
}

/* A retired slot must be reusable: the tombstone has to be claimed by a later
 * add for the same key, not shadow it. */
static void test_readd_after_removal(void) {
    FactIndex *f = fact_index_create();
    add_str(f, 10, 1, "p", "o");
    fact_index_remove(f, 10, 1, "p", FACT_OBJ_STRING, 0, "o");
    TEST_ASSERT_EQUAL_size_t(0, fact_index_facts(f));
    TEST_ASSERT_EQUAL_INT(0, add_str(f, 12, 1, "p", "o"));
    uint64_t *out = NULL;
    size_t n = 0;
    fact_index_by_subject(f, 1, "p", &out, &n);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(12, out[0]);
    free(out);
    fact_index_free(f);
}

/* ---- predicate accounting --------------------------------------------- */

/* The reported predicate count tracks predicates *in use*, so the same log
 * replayed into a fresh index reports the same number. (edge_index needed this
 * correction; the same trap is here.) */
static void test_predicate_count_tracks_use_not_history(void) {
    FactIndex *f = fact_index_create();
    add_str(f, 1, 1, "alpha", "o");
    add_str(f, 2, 1, "beta", "o");
    TEST_ASSERT_EQUAL_size_t(2, fact_index_predicates(f));

    add_str(f, 3, 2, "alpha", "o"); /* second use of alpha */
    TEST_ASSERT_EQUAL_size_t(2, fact_index_predicates(f));
    fact_index_remove(f, 3, 2, "alpha", FACT_OBJ_STRING, 0, "o");
    TEST_ASSERT_EQUAL_size_t(2, fact_index_predicates(f)); /* still in use */
    fact_index_remove(f, 1, 1, "alpha", FACT_OBJ_STRING, 0, "o");
    TEST_ASSERT_EQUAL_size_t(1, fact_index_predicates(f)); /* now released */

    /* re-using it counts once, not twice: the node was never freed */
    add_str(f, 4, 3, "alpha", "o");
    TEST_ASSERT_EQUAL_size_t(2, fact_index_predicates(f));
    fact_index_free(f);
}

/* An un-internable predicate is refused, not silently indexed as unqueryable —
 * the opposite of edge_index's kinds, and for a reason: an unnamed predicate
 * makes the fact unreachable by every pattern that mentions it. */
static void test_overlong_predicate_refused(void) {
    FactIndex *f = fact_index_create();
    char big[FACT_MAX_PREDICATE_LEN + 8];
    memset(big, 'p', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(-1, add_str(f, 1, 1, big, "o"));
    TEST_ASSERT_EQUAL_size_t(0, fact_index_facts(f));
    TEST_ASSERT_EQUAL_size_t(0, fact_index_predicates(f));

    char at[FACT_MAX_PREDICATE_LEN + 1];
    memset(at, 'q', FACT_MAX_PREDICATE_LEN);
    at[FACT_MAX_PREDICATE_LEN] = '\0';
    TEST_ASSERT_EQUAL_INT(0, add_str(f, 1, 1, at, "o"));
    TEST_ASSERT_EQUAL_size_t(1, fact_index_facts(f));

    /* an empty predicate is not a predicate */
    TEST_ASSERT_EQUAL_INT(-1, add_str(f, 2, 1, "", "o"));
    fact_index_free(f);
}

static void test_predicate_cap_refuses_rather_than_grows(void) {
    FactIndex *f = fact_index_create();
    char p[32];
    for (size_t i = 0; i < FACT_MAX_PREDICATES; i++) {
        snprintf(p, sizeof(p), "p_%zu", i);
        TEST_ASSERT_EQUAL_INT(0, add_str(f, i + 1, 1, p, "o"));
    }
    TEST_ASSERT_EQUAL_size_t(FACT_MAX_PREDICATES, fact_index_predicates(f));
    /* one past the cap: refused, and nothing about the index changes */
    size_t before = fact_index_facts(f);
    TEST_ASSERT_EQUAL_INT(-1, add_str(f, 999999, 1, "one_too_many", "o"));
    TEST_ASSERT_EQUAL_size_t(before, fact_index_facts(f));
    /* an already-interned predicate still works */
    TEST_ASSERT_EQUAL_INT(0, add_str(f, 999999, 2, "p_0", "o"));
    fact_index_free(f);
}

/* ---- malformed input and NULL tolerance -------------------------------- */

static void test_rejects_malformed_objects(void) {
    FactIndex *f = fact_index_create();
    /* a string object with no string */
    TEST_ASSERT_EQUAL_INT(
        -1, fact_index_add(f, 1, 1, "p", FACT_OBJ_STRING, 0, NULL));
    /* FACT_NONE is not an object kind */
    TEST_ASSERT_EQUAL_INT(-1, fact_index_add(f, 1, 1, "p", FACT_NONE, 0, NULL));
    /* no predicate */
    TEST_ASSERT_EQUAL_INT(-1,
                          fact_index_add(f, 1, 1, NULL, FACT_OBJ_ID, 2, NULL));
    TEST_ASSERT_EQUAL_size_t(0, fact_index_facts(f));
    fact_index_free(f);
}

static void test_null_index_is_inert(void) {
    uint64_t *out = (uint64_t *)0x1;
    size_t n = 99;
    TEST_ASSERT_EQUAL_INT(
        0, fact_index_add(NULL, 1, 1, "p", FACT_OBJ_ID, 2, NULL));
    fact_index_remove(NULL, 1, 1, "p", FACT_OBJ_ID, 2, NULL);
    TEST_ASSERT_EQUAL_INT(0, fact_index_by_subject(NULL, 1, "p", &out, &n));
    TEST_ASSERT_NULL(out);
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_INT(
        0, fact_index_by_object(NULL, FACT_OBJ_ID, 1, NULL, "p", &out, &n));
    TEST_ASSERT_EQUAL_INT(0, fact_index_by_predicate(NULL, "p", &out, &n));
    TEST_ASSERT_EQUAL_size_t(0, fact_index_facts(NULL));
    TEST_ASSERT_EQUAL_size_t(0, fact_index_predicates(NULL));
    TEST_ASSERT_EQUAL_size_t(0, fact_index_bytes(NULL));
    fact_index_free(NULL);
}

static void test_bytes_grows_with_content(void) {
    FactIndex *f = fact_index_create();
    size_t empty = fact_index_bytes(f);
    TEST_ASSERT_TRUE(empty > 0);
    for (uint64_t i = 0; i < 200; i++) {
        add_str(f, i + 1, i, "p", "o");
    }
    TEST_ASSERT_TRUE(fact_index_bytes(f) > empty);
    fact_index_free(f);
}

static void test_scale_stays_exact(void) {
    FactIndex *f = fact_index_create();
    const uint64_t N = 500;
    for (uint64_t i = 0; i < N; i++) {
        TEST_ASSERT_EQUAL_INT(0, add_id(f, 1000 + i, i, "part_of", i + 1));
    }
    TEST_ASSERT_EQUAL_size_t(N, fact_index_facts(f));
    for (uint64_t i = 0; i < N; i++) {
        uint64_t *out = NULL;
        size_t n = 0;
        TEST_ASSERT_EQUAL_INT(0,
                              fact_index_by_subject(f, i, "part_of", &out, &n));
        TEST_ASSERT_EQUAL_size_t(1, n);
        TEST_ASSERT_EQUAL_UINT64(1000 + i, out[0]);
        free(out);
        TEST_ASSERT_EQUAL_INT(0,
                              fact_index_by_object(f, FACT_OBJ_ID, i + 1, NULL,
                                                   "part_of", &out, &n));
        TEST_ASSERT_EQUAL_size_t(1, n);
        TEST_ASSERT_EQUAL_UINT64(1000 + i, out[0]);
        free(out);
    }
    fact_index_free(f);
}

/* ---- differential stress ---------------------------------------------- */

/* Three tables, tombstoned slots, sorted postings and interned predicates —
 * plenty of places for an off-by-one to hide as a fact that is findable one way
 * and not another. So: a deterministic op sequence against a brute-force model,
 * checking all three lookups agree with it. */

#define SUBJ_SPACE 24
#define OBJ_SPACE 12
#define N_PREDS 3
#define REF_MAX (SUBJ_SPACE * OBJ_SPACE * N_PREDS)

static const char *const PREDS[N_PREDS] = {"defaults_to", "part_of", "is_a"};
static const char *const OBJS[OBJ_SPACE] = {"a", "b", "c", "d", "e", "f",
                                            "g", "h", "i", "j", "k", "l"};

typedef struct {
    uint64_t rec;
    uint64_t subj;
    int pred;
    int obj;
    int live;
} RefFact;

static uint64_t lcg_state;
static uint64_t lcg(void) {
    lcg_state = lcg_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return lcg_state >> 17;
}

static void test_differential_against_reference_model(void) {
    FactIndex *f = fact_index_create();
    RefFact *ref = calloc(REF_MAX, sizeof(*ref));
    TEST_ASSERT_NOT_NULL(ref);
    size_t ref_n = 0;
    size_t live = 0;
    lcg_state = 0xFACEFACEFACEFACEULL;

    for (int step = 0; step < 12000; step++) {
        uint64_t op = lcg() % 100;
        uint64_t subj = lcg() % SUBJ_SPACE;
        int pred = (int)(lcg() % N_PREDS);
        int obj = (int)(lcg() % OBJ_SPACE);
        /* The record id is derived from the triple, so the same triple always
         * comes from the same record — which is what makes add idempotent and
         * remove targeted. */
        uint64_t rec = 1 + subj * (OBJ_SPACE * N_PREDS) +
                       (uint64_t)pred * OBJ_SPACE + (uint64_t)obj;

        if (op < 65) { /* add */
            TEST_ASSERT_EQUAL_INT(
                0, add_str(f, rec, subj, PREDS[pred], OBJS[obj]));
            int found = 0;
            for (size_t i = 0; i < ref_n; i++) {
                if (ref[i].rec == rec) {
                    if (!ref[i].live) {
                        ref[i].live = 1;
                        live++;
                    }
                    found = 1;
                    break;
                }
            }
            if (!found) {
                TEST_ASSERT_TRUE(ref_n < REF_MAX);
                ref[ref_n].rec = rec;
                ref[ref_n].subj = subj;
                ref[ref_n].pred = pred;
                ref[ref_n].obj = obj;
                ref[ref_n].live = 1;
                ref_n++;
                live++;
            }
        } else { /* remove */
            fact_index_remove(f, rec, subj, PREDS[pred], FACT_OBJ_STRING, 0,
                              OBJS[obj]);
            for (size_t i = 0; i < ref_n; i++) {
                if (ref[i].rec == rec && ref[i].live) {
                    ref[i].live = 0;
                    live--;
                    break;
                }
            }
        }
        TEST_ASSERT_EQUAL_size_t(live, fact_index_facts(f));

        /* Spot-check one subject and one object per step; checking every key
         * every step would make this quadratic for no more coverage. */
        uint64_t ps = lcg() % SUBJ_SPACE;
        int pp = (int)(lcg() % N_PREDS);
        uint64_t *out = NULL;
        size_t n = 0;
        TEST_ASSERT_EQUAL_INT(
            0, fact_index_by_subject(f, ps, PREDS[pp], &out, &n));
        assert_sorted_unique(out, n);
        size_t expect = 0;
        for (size_t i = 0; i < ref_n; i++) {
            if (ref[i].live && ref[i].subj == ps && ref[i].pred == pp) {
                expect++;
                TEST_ASSERT_TRUE(has(out, n, ref[i].rec));
            }
        }
        TEST_ASSERT_EQUAL_size_t(expect, n);
        free(out);

        int po = (int)(lcg() % OBJ_SPACE);
        TEST_ASSERT_EQUAL_INT(0, fact_index_by_object(f, FACT_OBJ_STRING, 0,
                                                      OBJS[po], PREDS[pp], &out,
                                                      &n));
        assert_sorted_unique(out, n);
        expect = 0;
        for (size_t i = 0; i < ref_n; i++) {
            if (ref[i].live && ref[i].obj == po && ref[i].pred == pp) {
                expect++;
                TEST_ASSERT_TRUE(has(out, n, ref[i].rec));
            }
        }
        TEST_ASSERT_EQUAL_size_t(expect, n);
        free(out);
    }

    /* Finally verify every predicate list, not just the sampled keys. */
    for (int p = 0; p < N_PREDS; p++) {
        uint64_t *out = NULL;
        size_t n = 0;
        TEST_ASSERT_EQUAL_INT(0,
                              fact_index_by_predicate(f, PREDS[p], &out, &n));
        assert_sorted_unique(out, n);
        size_t expect = 0;
        for (size_t i = 0; i < ref_n; i++) {
            if (ref[i].live && ref[i].pred == p) {
                expect++;
                TEST_ASSERT_TRUE(has(out, n, ref[i].rec));
            }
        }
        TEST_ASSERT_EQUAL_size_t(expect, n);
        free(out);
    }
    free(ref);
    fact_index_free(f);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_all_three_lookups_find_the_fact);
    RUN_TEST(test_wildcard_predicate);
    RUN_TEST(test_id_and_string_objects_do_not_collide);
    RUN_TEST(test_distinct_string_objects_stay_distinct);
    RUN_TEST(test_same_triple_from_two_records);
    RUN_TEST(test_add_is_idempotent);
    RUN_TEST(test_remove_clears_all_three_tables);
    RUN_TEST(test_readd_after_removal);
    RUN_TEST(test_predicate_count_tracks_use_not_history);
    RUN_TEST(test_overlong_predicate_refused);
    RUN_TEST(test_predicate_cap_refuses_rather_than_grows);
    RUN_TEST(test_rejects_malformed_objects);
    RUN_TEST(test_null_index_is_inert);
    RUN_TEST(test_bytes_grows_with_content);
    RUN_TEST(test_scale_stays_exact);
    RUN_TEST(test_differential_against_reference_model);
    return UNITY_END();
}
