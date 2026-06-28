/* Unit tests for the diagnostic logging level helpers (src/util/logging.c).
 *
 * These cover the pure, side-effect-free parts: name<->level mapping and the
 * global threshold accessor. Emission itself just writes to stderr and is
 * exercised by the contract tests / manual runs. */
#include "aegisdb/logging.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_from_string_known(void) {
    AegisLogLevel lvl;
    TEST_ASSERT_EQUAL_INT(0, aegis_log_level_from_string("error", &lvl));
    TEST_ASSERT_EQUAL_INT(AEGIS_LOG_ERROR, lvl);
    TEST_ASSERT_EQUAL_INT(0, aegis_log_level_from_string("warn", &lvl));
    TEST_ASSERT_EQUAL_INT(AEGIS_LOG_WARN, lvl);
    TEST_ASSERT_EQUAL_INT(0, aegis_log_level_from_string("info", &lvl));
    TEST_ASSERT_EQUAL_INT(AEGIS_LOG_INFO, lvl);
    TEST_ASSERT_EQUAL_INT(0, aegis_log_level_from_string("debug", &lvl));
    TEST_ASSERT_EQUAL_INT(AEGIS_LOG_DEBUG, lvl);
}

static void test_from_string_is_case_insensitive(void) {
    AegisLogLevel lvl;
    TEST_ASSERT_EQUAL_INT(0, aegis_log_level_from_string("WARN", &lvl));
    TEST_ASSERT_EQUAL_INT(AEGIS_LOG_WARN, lvl);
    /* "warning" is accepted as an alias for "warn". */
    TEST_ASSERT_EQUAL_INT(0, aegis_log_level_from_string("Warning", &lvl));
    TEST_ASSERT_EQUAL_INT(AEGIS_LOG_WARN, lvl);
}

static void test_from_string_rejects_unknown(void) {
    AegisLogLevel lvl = AEGIS_LOG_INFO;
    TEST_ASSERT_EQUAL_INT(-1, aegis_log_level_from_string("loud", &lvl));
    TEST_ASSERT_EQUAL_INT(-1, aegis_log_level_from_string("", &lvl));
    TEST_ASSERT_EQUAL_INT(-1, aegis_log_level_from_string(NULL, &lvl));
}

static void test_name_roundtrips(void) {
    const AegisLogLevel levels[] = {AEGIS_LOG_ERROR, AEGIS_LOG_WARN,
                                    AEGIS_LOG_INFO, AEGIS_LOG_DEBUG};
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        AegisLogLevel parsed;
        const char *name = aegis_log_level_name(levels[i]);
        TEST_ASSERT_EQUAL_INT(0, aegis_log_level_from_string(name, &parsed));
        TEST_ASSERT_EQUAL_INT(levels[i], parsed);
    }
}

static void test_set_get_level(void) {
    aegis_log_set_level(AEGIS_LOG_ERROR);
    TEST_ASSERT_EQUAL_INT(AEGIS_LOG_ERROR, aegis_log_get_level());
    aegis_log_set_level(AEGIS_LOG_DEBUG);
    TEST_ASSERT_EQUAL_INT(AEGIS_LOG_DEBUG, aegis_log_get_level());
    aegis_log_set_level(AEGIS_LOG_INFO); /* restore default */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_from_string_known);
    RUN_TEST(test_from_string_is_case_insensitive);
    RUN_TEST(test_from_string_rejects_unknown);
    RUN_TEST(test_name_roundtrips);
    RUN_TEST(test_set_get_level);
    return UNITY_END();
}