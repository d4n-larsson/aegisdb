/* Unit tests for the per-record usage-feedback index.
 *
 * The load/serialize round trip gets the most attention: this is the only index
 * that cannot be rebuilt from the log, so a checkpoint bug silently changes what
 * `forget` deletes rather than failing loudly. */
#include <stdlib.h>
#include <string.h>

#include "aegisdb/usage_index.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_untracked_id_reads_as_absent(void) {
    UsageIndex *u = usage_index_create();
    uint32_t c = 99;
    uint64_t last = 99;
    TEST_ASSERT_EQUAL_INT(-1, usage_index_get(u, 42, &c, &last));
    /* Recording an untracked id is a no-op, not a crash or an implicit insert:
     * the read path must never allocate. */
    usage_index_record(u, 42, 1000);
    TEST_ASSERT_EQUAL_INT(-1, usage_index_get(u, 42, NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(0, usage_index_count(u));
    usage_index_free(u);
}

static void test_track_then_record(void) {
    UsageIndex *u = usage_index_create();
    TEST_ASSERT_EQUAL_INT(0, usage_index_track(u, 7));
    uint32_t c = 99;
    uint64_t last = 99;
    /* Tracked but never recalled reads as a real zero, distinct from absent. */
    TEST_ASSERT_EQUAL_INT(0, usage_index_get(u, 7, &c, &last));
    TEST_ASSERT_EQUAL_UINT32(0, c);
    TEST_ASSERT_EQUAL_UINT64(0, last);

    usage_index_record(u, 7, 1000);
    usage_index_record(u, 7, 2000);
    TEST_ASSERT_EQUAL_INT(0, usage_index_get(u, 7, &c, &last));
    TEST_ASSERT_EQUAL_UINT32(2, c);
    TEST_ASSERT_EQUAL_UINT64(2000, last); /* most recent wins */
    TEST_ASSERT_EQUAL_UINT64(2, usage_index_total_recalls(u));
    usage_index_free(u);
}

static void test_track_is_idempotent_and_preserves_counters(void) {
    UsageIndex *u = usage_index_create();
    usage_index_track(u, 1);
    usage_index_record(u, 1, 500);
    /* Re-tracking must not reset the history — recovery re-establishes slots for
     * records whose counters are then restored from the checkpoint. */
    TEST_ASSERT_EQUAL_INT(0, usage_index_track(u, 1));
    uint32_t c = 0;
    TEST_ASSERT_EQUAL_INT(0, usage_index_get(u, 1, &c, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, c);
    TEST_ASSERT_EQUAL_size_t(1, usage_index_count(u));
    usage_index_free(u);
}

static void test_untrack_drops_the_record(void) {
    UsageIndex *u = usage_index_create();
    usage_index_track(u, 5);
    usage_index_record(u, 5, 100);
    usage_index_untrack(u, 5);
    TEST_ASSERT_EQUAL_INT(-1, usage_index_get(u, 5, NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(0, usage_index_count(u));
    TEST_ASSERT_EQUAL_UINT64(0, usage_index_total_recalls(u));
    /* An id re-tracked after removal starts clean. */
    usage_index_track(u, 5);
    uint32_t c = 99;
    TEST_ASSERT_EQUAL_INT(0, usage_index_get(u, 5, &c, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, c);
    usage_index_free(u);
}

static void test_growth_preserves_every_counter(void) {
    UsageIndex *u = usage_index_create();
    for (uint64_t i = 1; i <= 2000; i++) { /* forces several rehashes */
        TEST_ASSERT_EQUAL_INT(0, usage_index_track(u, i));
        for (uint64_t k = 0; k < (i % 5); k++) {
            usage_index_record(u, i, 1000 + i);
        }
    }
    TEST_ASSERT_EQUAL_size_t(2000, usage_index_count(u));
    for (uint64_t i = 1; i <= 2000; i++) {
        uint32_t c = 0;
        uint64_t last = 0;
        TEST_ASSERT_EQUAL_INT(0, usage_index_get(u, i, &c, &last));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)(i % 5), c);
        TEST_ASSERT_EQUAL_UINT64((i % 5) ? 1000 + i : 0, last);
    }
    usage_index_free(u);
}

/* Churn must not leak slots: repeated track/untrack of distinct ids should
 * reuse tombstones rather than growing the table without bound. */
static void test_churn_reuses_slots(void) {
    UsageIndex *u = usage_index_create();
    for (uint64_t i = 1; i <= 5000; i++) {
        usage_index_track(u, i);
        usage_index_record(u, i, i);
        usage_index_untrack(u, i);
    }
    TEST_ASSERT_EQUAL_size_t(0, usage_index_count(u));
    /* A handful of live entries should not need a huge table. */
    TEST_ASSERT_TRUE(usage_index_bytes(u) < 1u << 20);
    usage_index_free(u);
}

/* --- checkpoint round trip ---------------------------------------------- */

static void test_serialize_load_round_trip(void) {
    UsageIndex *a = usage_index_create();
    for (uint64_t i = 1; i <= 50; i++) {
        usage_index_track(a, i);
        for (uint64_t k = 0; k < i % 4; k++) {
            usage_index_record(a, i, 7000 + i);
        }
    }
    size_t len = 0;
    uint8_t *buf = usage_index_serialize(a, &len);
    TEST_ASSERT_NOT_NULL(buf);

    /* Recovery order: the log establishes the live slots, then the checkpoint
     * restores counters onto them. */
    UsageIndex *b = usage_index_create();
    for (uint64_t i = 1; i <= 50; i++) {
        usage_index_track(b, i);
    }
    TEST_ASSERT_EQUAL_INT(0, usage_index_load_buf(b, buf, len));
    for (uint64_t i = 1; i <= 50; i++) {
        uint32_t c = 0;
        uint64_t last = 0;
        TEST_ASSERT_EQUAL_INT(0, usage_index_get(b, i, &c, &last));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)(i % 4), c);
        TEST_ASSERT_EQUAL_UINT64((i % 4) ? 7000 + i : 0, last);
    }
    free(buf);
    usage_index_free(a);
    usage_index_free(b);
}

/* A record deleted since the checkpoint must not be resurrected by loading it. */
static void test_load_ignores_ids_that_are_no_longer_live(void) {
    UsageIndex *a = usage_index_create();
    usage_index_track(a, 1);
    usage_index_track(a, 2);
    usage_index_record(a, 1, 100);
    usage_index_record(a, 2, 200);
    size_t len = 0;
    uint8_t *buf = usage_index_serialize(a, &len);
    TEST_ASSERT_NOT_NULL(buf);

    UsageIndex *b = usage_index_create();
    usage_index_track(b, 1); /* id 2 was deleted while the server was down */
    TEST_ASSERT_EQUAL_INT(0, usage_index_load_buf(b, buf, len));
    TEST_ASSERT_EQUAL_size_t(1, usage_index_count(b));
    TEST_ASSERT_EQUAL_INT(-1, usage_index_get(b, 2, NULL, NULL));
    uint32_t c = 0;
    TEST_ASSERT_EQUAL_INT(0, usage_index_get(b, 1, &c, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, c);
    free(buf);
    usage_index_free(a);
    usage_index_free(b);
}

/* Never-recalled records carry no information, so the image should skip them. */
static void test_serialize_skips_zero_counts(void) {
    UsageIndex *u = usage_index_create();
    for (uint64_t i = 1; i <= 100; i++) {
        usage_index_track(u, i);
    }
    usage_index_record(u, 42, 1);
    size_t len = 0;
    uint8_t *buf = usage_index_serialize(u, &len);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_size_t(16 + 20, len); /* header + exactly one entry */
    free(buf);
    usage_index_free(u);
}

static void test_load_rejects_garbage(void) {
    UsageIndex *u = usage_index_create();
    usage_index_track(u, 1);
    uint8_t junk[64];
    memset(junk, 0xAB, sizeof(junk));
    TEST_ASSERT_EQUAL_INT(-1, usage_index_load_buf(u, junk, sizeof(junk)));
    TEST_ASSERT_EQUAL_INT(-1, usage_index_load_buf(u, NULL, 0));

    /* A truncated image (header claims more entries than the bytes hold) must be
     * rejected rather than read past the buffer. */
    size_t len = 0;
    usage_index_record(u, 1, 5);
    uint8_t *buf = usage_index_serialize(u, &len);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_INT(-1, usage_index_load_buf(u, buf, len - 4));
    free(buf);
    usage_index_free(u);
}

/* Every entry point tolerates a NULL index (--no-usage-feedback), so call sites
 * stay unconditional. */
static void test_null_index_is_inert(void) {
    TEST_ASSERT_EQUAL_INT(0, usage_index_track(NULL, 1));
    usage_index_untrack(NULL, 1);
    usage_index_record(NULL, 1, 1);
    TEST_ASSERT_EQUAL_INT(-1, usage_index_get(NULL, 1, NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(0, usage_index_count(NULL));
    TEST_ASSERT_EQUAL_size_t(0, usage_index_bytes(NULL));
    TEST_ASSERT_EQUAL_UINT64(0, usage_index_total_recalls(NULL));
    size_t len = 1;
    TEST_ASSERT_NULL(usage_index_serialize(NULL, &len));
    TEST_ASSERT_EQUAL_size_t(0, len);
    TEST_ASSERT_EQUAL_INT(-1, usage_index_load_buf(NULL, NULL, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_untracked_id_reads_as_absent);
    RUN_TEST(test_track_then_record);
    RUN_TEST(test_track_is_idempotent_and_preserves_counters);
    RUN_TEST(test_untrack_drops_the_record);
    RUN_TEST(test_growth_preserves_every_counter);
    RUN_TEST(test_churn_reuses_slots);
    RUN_TEST(test_serialize_load_round_trip);
    RUN_TEST(test_load_ignores_ids_that_are_no_longer_live);
    RUN_TEST(test_serialize_skips_zero_counts);
    RUN_TEST(test_load_rejects_garbage);
    RUN_TEST(test_null_index_is_inert);
    return UNITY_END();
}