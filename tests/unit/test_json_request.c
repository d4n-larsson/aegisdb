/* Tests for the generic cJSON field accessors in json_request.c. */
#include <stdint.h>

#include "aegisdb/json_request.h"
#include "cJSON.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_jr_u64_valid(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "n", 12345.0);
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(0, jr_u64(o, "n", &v));
    TEST_ASSERT_EQUAL_UINT64(12345, v);
    cJSON_Delete(o);
}

static void test_jr_u64_rejects_negative(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "n", -1.0);
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(-1, jr_u64(o, "n", &v));
    cJSON_Delete(o);
}

/* A double >= 2^64 must be rejected: casting it to uint64_t is undefined
 * behaviour and would otherwise yield a garbage huge integer. */
static void test_jr_u64_rejects_overflow(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "n", 1e30); /* far above UINT64_MAX */
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(-1, jr_u64(o, "n", &v));
    cJSON_Delete(o);
}

static void test_jr_u64_missing_or_nonnumber(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "s", "nope");
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(-1, jr_u64(o, "missing", &v));
    TEST_ASSERT_EQUAL_INT(-1, jr_u64(o, "s", &v));
    cJSON_Delete(o);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_jr_u64_valid);
    RUN_TEST(test_jr_u64_rejects_negative);
    RUN_TEST(test_jr_u64_rejects_overflow);
    RUN_TEST(test_jr_u64_missing_or_nonnumber);
    return UNITY_END();
}