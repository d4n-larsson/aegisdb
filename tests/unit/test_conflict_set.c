/* Unit tests for the flagged-contradiction set — ROADMAP 5.4 §6.
 *
 * This module has one job and one dangerous failure. The job is to keep what
 * the inference pass already found so it can be listed. The failure is
 * reporting a contradiction that is no longer there: a stale pair costs an
 * adjudicator a model call and, worse, invites it to tombstone a record over a
 * conflict somebody already resolved. So the tests below pin the identity of a
 * pair (one pair regardless of which order it was found in), the namespace
 * filter that carries tenant isolation, and the two shortfalls — a short page
 * and a full set — staying distinguishable. */
#include <stdio.h>
#include <string.h>

#include "aegisdb/conflict_set.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static size_t list_all(const ConflictSet *cs, const char *ns, ConflictPair *out,
                       size_t max, size_t *total) {
    return conflict_set_list(cs, ns, out, max, total);
}

static void test_add_then_list(void) {
    ConflictSet *cs = conflict_set_create();
    TEST_ASSERT_NOT_NULL(cs);
    TEST_ASSERT_EQUAL_INT(0, conflict_set_add(cs, 2, 3, "acme", "defaults_to",
                                              "defaults_to", "cardinality"));
    TEST_ASSERT_EQUAL_UINT(1, conflict_set_count(cs));

    ConflictPair out[4];
    size_t total = 0;
    TEST_ASSERT_EQUAL_UINT(1, list_all(cs, NULL, out, 4, &total));
    TEST_ASSERT_EQUAL_UINT(1, total);
    TEST_ASSERT_EQUAL_UINT64(2, out[0].a);
    TEST_ASSERT_EQUAL_UINT64(3, out[0].b);
    TEST_ASSERT_EQUAL_STRING("acme", out[0].ns);
    TEST_ASSERT_EQUAL_STRING("cardinality", out[0].reason);
    conflict_set_free(cs);
}

/* A pair has one identity regardless of which record the scan reached first.
 * Without this the same contradiction found by the cardinality scan and by a
 * self-referential mutex declaration reads as two, and an adjudicator spends
 * two verdicts to tombstone one record — the second finding its loser gone. */
static void test_a_pair_has_one_identity(void) {
    ConflictSet *cs = conflict_set_create();
    TEST_ASSERT_EQUAL_INT(
        0, conflict_set_add(cs, 7, 4, "ns", "p_hi", "p_lo", "mutex_with"));
    /* 1, not 0: the caller counts contradictions rather than rule firings, and
     * needs to tell "already recorded this pass" from "recorded". A pair the
     * cap rejected returns -1 and IS still counted, so the gauge stays exact
     * past the cap — the two cases must not collapse. */
    TEST_ASSERT_EQUAL_INT(
        1, conflict_set_add(cs, 4, 7, "ns", "p_lo", "p_hi", "mutex_with"));
    TEST_ASSERT_EQUAL_UINT(1, conflict_set_count(cs));

    ConflictPair out[2];
    size_t total = 0;
    list_all(cs, NULL, out, 2, &total);
    /* Normalized low-id-first, and the predicates travel with their records —
     * swapping the ids without swapping the predicates would attribute each
     * side's predicate to the other. */
    TEST_ASSERT_EQUAL_UINT64(4, out[0].a);
    TEST_ASSERT_EQUAL_UINT64(7, out[0].b);
    TEST_ASSERT_EQUAL_STRING("p_lo", out[0].predicate_a);
    TEST_ASSERT_EQUAL_STRING("p_hi", out[0].predicate_b);
    conflict_set_free(cs);
}

static void test_degenerate_pairs_are_refused(void) {
    ConflictSet *cs = conflict_set_create();
    TEST_ASSERT_EQUAL_INT(-1, conflict_set_add(cs, 0, 3, "ns", "p", "p", "r"));
    TEST_ASSERT_EQUAL_INT(-1, conflict_set_add(cs, 3, 0, "ns", "p", "p", "r"));
    /* A record does not contradict itself; storing one would hand an
     * adjudicator a pair whose two sides are the same record. */
    TEST_ASSERT_EQUAL_INT(-1, conflict_set_add(cs, 5, 5, "ns", "p", "p", "r"));
    TEST_ASSERT_EQUAL_UINT(0, conflict_set_count(cs));
    conflict_set_free(cs);
}

/* The filter that carries tenant isolation: the fact indexes are server-wide,
 * so this is the property a naive implementation gets wrong. */
static void test_namespace_filter(void) {
    ConflictSet *cs = conflict_set_create();
    conflict_set_add(cs, 2, 3, "acme", "p", "p", "cardinality");
    conflict_set_add(cs, 5, 6, "beta", "p", "p", "cardinality");
    conflict_set_add(cs, 8, 9, "", "p", "p", "cardinality");

    ConflictPair out[8];
    size_t total = 0;
    TEST_ASSERT_EQUAL_UINT(1, list_all(cs, "acme", out, 8, &total));
    TEST_ASSERT_EQUAL_UINT(1, total);
    TEST_ASSERT_EQUAL_UINT64(2, out[0].a);

    /* NULL and "" both mean "every namespace" — the unrestricted caller. */
    TEST_ASSERT_EQUAL_UINT(3, list_all(cs, NULL, out, 8, &total));
    TEST_ASSERT_EQUAL_UINT(3, list_all(cs, "", out, 8, &total));

    TEST_ASSERT_EQUAL_UINT(0, list_all(cs, "nobody", out, 8, &total));
    TEST_ASSERT_EQUAL_UINT(0, total);
    conflict_set_free(cs);
}

/* `total` counts what matched, not what fitted, so a caller can tell "there
 * are no more" from "you asked for fewer" — and a max of 0 with no buffer is
 * the "how many are there?" probe. Answering 0 there would be a false
 * all-clear, which is the one answer this module must never give. */
static void test_total_counts_matches_not_writes(void) {
    ConflictSet *cs = conflict_set_create();
    for (uint64_t i = 0; i < 5; i++) {
        conflict_set_add(cs, 100 + i * 2, 101 + i * 2, "acme", "p", "p", "r");
    }
    ConflictPair out[2];
    size_t total = 0;
    TEST_ASSERT_EQUAL_UINT(2, list_all(cs, NULL, out, 2, &total));
    TEST_ASSERT_EQUAL_UINT(5, total);

    total = 0;
    TEST_ASSERT_EQUAL_UINT(0, conflict_set_list(cs, NULL, NULL, 0, &total));
    TEST_ASSERT_EQUAL_UINT(5, total);
    conflict_set_free(cs);
}

/* Two different shortfalls. A short page is the caller's own doing and a
 * bigger limit fixes it; a full set means the pass found more than the list
 * retains, and asking again cannot help. Conflating them would tell an
 * operator to page for data that was never kept. */
static void test_truncation_is_not_paging(void) {
    ConflictSet *cs = conflict_set_create();
    for (uint64_t i = 0; i < CONFLICT_SET_MAX; i++) {
        TEST_ASSERT_EQUAL_INT(
            0, conflict_set_add(cs, 2 + i * 2, 3 + i * 2, "ns", "p", "p", "r"));
    }
    TEST_ASSERT_EQUAL_INT(0, conflict_set_truncated(cs));
    TEST_ASSERT_EQUAL_INT(
        -1, conflict_set_add(cs, 99999, 100000, "ns", "p", "p", "r"));
    TEST_ASSERT_EQUAL_INT(1, conflict_set_truncated(cs));
    TEST_ASSERT_EQUAL_UINT(CONFLICT_SET_MAX, conflict_set_count(cs));
    conflict_set_free(cs);
}

/* Replaced whole, never accumulated: a contradiction resolved since the last
 * pass has to stop being reported. */
static void test_clear_resets_truncation_too(void) {
    ConflictSet *cs = conflict_set_create();
    for (uint64_t i = 0; i <= CONFLICT_SET_MAX; i++) {
        conflict_set_add(cs, 2 + i * 2, 3 + i * 2, "ns", "p", "p", "r");
    }
    TEST_ASSERT_EQUAL_INT(1, conflict_set_truncated(cs));
    conflict_set_clear(cs);
    TEST_ASSERT_EQUAL_UINT(0, conflict_set_count(cs));
    TEST_ASSERT_EQUAL_INT(0, conflict_set_truncated(cs));
    conflict_set_free(cs);
}

/* Bounded rather than truncated-and-wrong. Every field is already capped at
 * the write path, so this is unreachable in practice — but a shortened
 * predicate in a report beats dropping the contradiction it names. */
static void test_overlong_fields_truncate_and_terminate(void) {
    char pred[CONFLICT_PREDICATE_MAX * 3];
    memset(pred, 'p', sizeof(pred) - 1);
    pred[sizeof(pred) - 1] = '\0';

    ConflictSet *cs = conflict_set_create();
    TEST_ASSERT_EQUAL_INT(
        0, conflict_set_add(cs, 2, 3, "ns", pred, pred, "cardinality"));
    ConflictPair out[1];
    size_t total = 0;
    list_all(cs, NULL, out, 1, &total);
    TEST_ASSERT_EQUAL_UINT(CONFLICT_PREDICATE_MAX, strlen(out[0].predicate_a));
    conflict_set_free(cs);
}

/* Every entry point tolerates NULL, so the pass can call them unconditionally
 * when allocation failed — losing the list, never the scan or the gauge. */
static void test_null_set_is_inert(void) {
    ConflictPair out[1];
    size_t total = 7;
    TEST_ASSERT_EQUAL_INT(-1,
                          conflict_set_add(NULL, 2, 3, "ns", "p", "p", "r"));
    TEST_ASSERT_EQUAL_UINT(0, conflict_set_count(NULL));
    TEST_ASSERT_EQUAL_INT(0, conflict_set_truncated(NULL));
    TEST_ASSERT_EQUAL_UINT(0, list_all(NULL, NULL, out, 1, &total));
    TEST_ASSERT_EQUAL_UINT(0, total);
    conflict_set_clear(NULL);
    conflict_set_free(NULL);
}

static void test_null_ns_is_stored_as_empty(void) {
    ConflictSet *cs = conflict_set_create();
    conflict_set_add(cs, 2, 3, NULL, NULL, NULL, NULL);
    ConflictPair out[1];
    size_t total = 0;
    TEST_ASSERT_EQUAL_UINT(1, list_all(cs, NULL, out, 1, &total));
    TEST_ASSERT_EQUAL_STRING("", out[0].ns);
    TEST_ASSERT_EQUAL_STRING("", out[0].predicate_a);
    TEST_ASSERT_EQUAL_STRING("", out[0].reason);
    /* An unnamespaced pair belongs to no tenant, so a namespaced caller must
     * not see it. */
    TEST_ASSERT_EQUAL_UINT(0, list_all(cs, "acme", out, 1, &total));
    conflict_set_free(cs);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add_then_list);
    RUN_TEST(test_a_pair_has_one_identity);
    RUN_TEST(test_degenerate_pairs_are_refused);
    RUN_TEST(test_namespace_filter);
    RUN_TEST(test_total_counts_matches_not_writes);
    RUN_TEST(test_truncation_is_not_paging);
    RUN_TEST(test_clear_resets_truncation_too);
    RUN_TEST(test_overlong_fields_truncate_and_terminate);
    RUN_TEST(test_null_set_is_inert);
    RUN_TEST(test_null_ns_is_stored_as_empty);
    return UNITY_END();
}
