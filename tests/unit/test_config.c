/* Unit tests for CLI argument parsing, focused on numeric overflow rejection. */
#include "aegisdb/config.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_rejects_overflow_port(void) {
    Config cfg;
    config_defaults(&cfg);
    char *argv[] = {"aegisdb", "--port", "99999999999999999999"};
    TEST_ASSERT_EQUAL_INT(-1, config_parse_args(&cfg, 3, argv));
    config_free(&cfg);
}

/* In range for `long` on LP64 but out of range for `int` — must still reject. */
static void test_rejects_out_of_int_range_workers(void) {
    Config cfg;
    config_defaults(&cfg);
    char *argv[] = {"aegisdb", "--workers", "5000000000"};
    TEST_ASSERT_EQUAL_INT(-1, config_parse_args(&cfg, 3, argv));
    config_free(&cfg);
}

static void test_rejects_overflow_max_payload(void) {
    Config cfg;
    config_defaults(&cfg);
    char *argv[] = {"aegisdb", "--max-payload", "999999999999999999999999"};
    TEST_ASSERT_EQUAL_INT(-1, config_parse_args(&cfg, 3, argv));
    config_free(&cfg);
}

static void test_accepts_valid(void) {
    Config cfg;
    config_defaults(&cfg);
    char *argv[] = {"aegisdb", "--port", "9999", "--io-threads", "8"};
    TEST_ASSERT_EQUAL_INT(0, config_parse_args(&cfg, 5, argv));
    TEST_ASSERT_EQUAL_INT(9999, cfg.listen_port);
    TEST_ASSERT_EQUAL_INT(8, cfg.io_threads);
    config_free(&cfg);
}

/* Compaction runs on a timer by default; --compact-sec overrides, 0 disables. */
static void test_compact_sec(void) {
    Config cfg;
    config_defaults(&cfg);
    TEST_ASSERT_EQUAL_UINT(300, cfg.compact_sec); /* enabled by default */
    char *argv[] = {"aegisdb", "--compact-sec", "0"};
    TEST_ASSERT_EQUAL_INT(0, config_parse_args(&cfg, 3, argv));
    TEST_ASSERT_EQUAL_UINT(0, cfg.compact_sec);
    config_free(&cfg);
}

/* --workers remains a back-compat alias for --io-threads. */
static void test_workers_alias(void) {
    Config cfg;
    config_defaults(&cfg);
    char *argv[] = {"aegisdb", "--workers", "5"};
    TEST_ASSERT_EQUAL_INT(0, config_parse_args(&cfg, 3, argv));
    TEST_ASSERT_EQUAL_INT(5, cfg.io_threads);
    config_free(&cfg);
}

/* --ann-shard-target: defaults to 0 (built-in default), parses a size, rejects
 * a non-numeric value. */
static void test_ann_shard_target(void) {
    Config cfg;
    config_defaults(&cfg);
    TEST_ASSERT_EQUAL_UINT(0, cfg.ann_shard_target); /* default: built-in */
    char *ok[] = {"aegisdb", "--ann-shard-target", "50000"};
    TEST_ASSERT_EQUAL_INT(0, config_parse_args(&cfg, 3, ok));
    TEST_ASSERT_EQUAL_UINT(50000, cfg.ann_shard_target);
    config_free(&cfg);

    config_defaults(&cfg);
    char *bad[] = {"aegisdb", "--ann-shard-target", "abc"};
    TEST_ASSERT_EQUAL_INT(-1, config_parse_args(&cfg, 3, bad));
    config_free(&cfg);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rejects_overflow_port);
    RUN_TEST(test_rejects_out_of_int_range_workers);
    RUN_TEST(test_rejects_overflow_max_payload);
    RUN_TEST(test_accepts_valid);
    RUN_TEST(test_compact_sec);
    RUN_TEST(test_workers_alias);
    RUN_TEST(test_ann_shard_target);
    return UNITY_END();
}