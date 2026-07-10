/* Unit tests for durability modes: mode parsing, the effective fsync-count
 * mapping, and the log's interval-flush helpers. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/config.h"
#include "aegisdb/log.h"
#include "unity.h"

static char g_path[256];

void setUp(void) {
    snprintf(g_path, sizeof(g_path), "/tmp/aegis_test_dura_%d.log", (int)getpid());
    remove(g_path);
}
void tearDown(void) { remove(g_path); }

static void test_mode_name_roundtrip(void) {
    int m = -1;
    TEST_ASSERT_EQUAL_INT(0, aegis_durability_from_string("sync", &m));
    TEST_ASSERT_EQUAL_INT(AEGIS_DURABILITY_SYNC, m);
    TEST_ASSERT_EQUAL_STRING("sync", aegis_durability_name(m));

    TEST_ASSERT_EQUAL_INT(0, aegis_durability_from_string("batch", &m));
    TEST_ASSERT_EQUAL_INT(AEGIS_DURABILITY_BATCH, m);
    TEST_ASSERT_EQUAL_STRING("batch", aegis_durability_name(m));

    TEST_ASSERT_EQUAL_INT(0, aegis_durability_from_string("interval", &m));
    TEST_ASSERT_EQUAL_INT(AEGIS_DURABILITY_INTERVAL, m);
    TEST_ASSERT_EQUAL_STRING("interval", aegis_durability_name(m));
}

static void test_mode_parse_rejects_garbage(void) {
    int m = AEGIS_DURABILITY_BATCH;
    TEST_ASSERT_EQUAL_INT(-1, aegis_durability_from_string("fsync", &m));
    TEST_ASSERT_EQUAL_INT(-1, aegis_durability_from_string("", &m));
    TEST_ASSERT_EQUAL_INT(-1, aegis_durability_from_string(NULL, &m));
    /* unchanged on failure */
    TEST_ASSERT_EQUAL_INT(AEGIS_DURABILITY_BATCH, m);
}

/* The mode determines the count threshold handed to the log: SYNC flushes
 * every write, INTERVAL never flushes on count, BATCH uses the configured
 * batch size. */
static void test_effective_fsync_batch(void) {
    Config cfg;
    config_defaults(&cfg);
    TEST_ASSERT_EQUAL_INT(AEGIS_DURABILITY_INTERVAL, cfg.durability);
    cfg.fsync_batch_size = 1000;

    cfg.durability = AEGIS_DURABILITY_SYNC;
    TEST_ASSERT_EQUAL_size_t(1, config_effective_fsync_batch(&cfg));

    cfg.durability = AEGIS_DURABILITY_BATCH;
    TEST_ASSERT_EQUAL_size_t(1000, config_effective_fsync_batch(&cfg));

    cfg.durability = AEGIS_DURABILITY_INTERVAL;
    TEST_ASSERT_EQUAL_size_t(SIZE_MAX, config_effective_fsync_batch(&cfg));

    config_free(&cfg);
}

/* log_append no longer fsyncs inline (so the fsync can run off the index lock);
 * the deferred batched flush log_fsync_if_batched does it. In SYNC mode
 * (batch == 1) a single append is immediately due, so the flush clears it. */
static void test_sync_batch_leaves_nothing_pending(void) {
    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 1, NULL, NULL));
    uint64_t off;
    TEST_ASSERT_EQUAL_INT(0, log_append(&lf, (const uint8_t *)"x", 1, &off));
    TEST_ASSERT_TRUE(log_flush_pending(&lf)); /* append defers the fsync */
    log_fsync_if_batched(&lf);
    TEST_ASSERT_FALSE(log_flush_pending(&lf)); /* batch==1 -> flushed */
    log_close(&lf);
}

/* INTERVAL mode (batch == SIZE_MAX) accrues unflushed appends until an
 * explicit fsync clears them — this is what the maintenance thread drives. */
static void test_interval_pending_until_explicit_fsync(void) {
    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, SIZE_MAX, NULL, NULL));
    uint64_t off;
    TEST_ASSERT_EQUAL_INT(0, log_append(&lf, (const uint8_t *)"a", 1, &off));
    TEST_ASSERT_EQUAL_INT(0, log_append(&lf, (const uint8_t *)"b", 1, &off));
    TEST_ASSERT_TRUE(log_flush_pending(&lf));
    log_fsync(&lf);
    TEST_ASSERT_FALSE(log_flush_pending(&lf));
    log_close(&lf);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mode_name_roundtrip);
    RUN_TEST(test_mode_parse_rejects_garbage);
    RUN_TEST(test_effective_fsync_batch);
    RUN_TEST(test_sync_batch_leaves_nothing_pending);
    RUN_TEST(test_interval_pending_until_explicit_fsync);
    return UNITY_END();
}