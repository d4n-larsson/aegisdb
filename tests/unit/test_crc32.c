/* Unit tests for the CRC32 (IEEE 802.3) utility. */
#include <string.h>

#include "aegisdb/crc32.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Known IEEE CRC32 of the ASCII string "123456789" is 0xCBF43926. */
static void test_known_vector(void) {
    const char *s = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32_compute(s, strlen(s)));
}

static void test_empty_is_zero(void) {
    TEST_ASSERT_EQUAL_HEX32(0u, crc32_compute("", 0));
    TEST_ASSERT_EQUAL_HEX32(0u, crc32_compute(NULL, 0));
}

/* The incremental API fed in two chunks must equal the one-shot result. */
static void test_incremental_matches_oneshot(void) {
    const char *full = "the quick brown fox";
    uint32_t one = crc32_compute(full, strlen(full));

    uint32_t inc = crc32_update(0, "the quick ", 10);
    inc = crc32_update(inc, "brown fox", 9);
    TEST_ASSERT_EQUAL_HEX32(one, inc);
}

static void test_different_data_differs(void) {
    TEST_ASSERT_NOT_EQUAL(crc32_compute("aegis", 5), crc32_compute("aegix", 5));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_known_vector);
    RUN_TEST(test_empty_is_zero);
    RUN_TEST(test_incremental_matches_oneshot);
    RUN_TEST(test_different_data_differs);
    return UNITY_END();
}