/* Unit tests for the status-code and memory-type string helpers.
 *
 * These map every public status onto the wire-protocol code string clients match
 * on, so a new enum member added without a switch arm (falling through to
 * "INTERNAL") is a silent wire-contract break. The tests below pin every arm,
 * including the AEGIS_OK and AEGIS_ERR_INTERNAL ends that the request paths
 * never take, and assert that no two statuses share a code. */
#include <string.h>

#include "aegisdb/errors.h"
#include "aegisdb/types.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Every status, paired with the code string the wire protocol documents. Kept in
 * enum order so a new member shows up as a gap here too. */
static const struct {
    aegis_status_t s;
    const char *code;
} k_codes[] = {
    {AEGIS_OK, "OK"},
    {AEGIS_ERR_INVALID_REQUEST, "INVALID_REQUEST"},
    {AEGIS_ERR_NOT_FOUND, "NOT_FOUND"},
    {AEGIS_ERR_PAYLOAD_TOO_LARGE, "PAYLOAD_TOO_LARGE"},
    {AEGIS_ERR_IMMUTABLE, "IMMUTABLE"},
    {AEGIS_ERR_NOT_READY, "NOT_READY"},
    {AEGIS_ERR_UNAUTHORIZED, "UNAUTHORIZED"},
    {AEGIS_ERR_FORBIDDEN, "FORBIDDEN"},
    {AEGIS_ERR_QUOTA_EXCEEDED, "QUOTA_EXCEEDED"},
    {AEGIS_ERR_RATE_LIMITED, "RATE_LIMITED"},
    {AEGIS_ERR_READ_ONLY, "READ_ONLY"},
    {AEGIS_ERR_MEMORY_LIMIT, "MEMORY_LIMIT"},
    {AEGIS_ERR_INTERNAL, "INTERNAL"},
};
#define N_CODES (sizeof(k_codes) / sizeof(k_codes[0]))

static void test_status_code_strings(void) {
    for (size_t i = 0; i < N_CODES; i++) {
        TEST_ASSERT_EQUAL_STRING(k_codes[i].code,
                                 aegis_status_code(k_codes[i].s));
    }
}

/* The table must cover the whole enum: AEGIS_INTERNAL is the last member, so its
 * numeric value is the count minus one. A member added before it without a table
 * entry (or a switch arm) trips this. */
static void test_status_table_covers_enum(void) {
    TEST_ASSERT_EQUAL_size_t((size_t)AEGIS_ERR_INTERNAL + 1, N_CODES);
}

/* Codes are what clients branch on, so no two statuses may collapse onto one —
 * a missing switch arm shows up here as a duplicate "INTERNAL". */
static void test_status_codes_are_distinct(void) {
    for (size_t i = 0; i < N_CODES; i++) {
        for (size_t j = i + 1; j < N_CODES; j++) {
            const char *a = aegis_status_code(k_codes[i].s);
            const char *b = aegis_status_code(k_codes[j].s);
            TEST_ASSERT_FALSE_MESSAGE(strcmp(a, b) == 0,
                                      "two statuses share a code string");
        }
    }
}

/* Messages are operator-facing, not matched on, so only the invariants matter:
 * always non-NULL and never empty (a NULL would crash the JSON encoder). */
static void test_status_messages_present(void) {
    for (size_t i = 0; i < N_CODES; i++) {
        const char *m = aegis_status_message(k_codes[i].s);
        TEST_ASSERT_NOT_NULL(m);
        TEST_ASSERT_TRUE(m[0] != '\0');
    }
}

/* Out-of-range values reach these helpers only via a bug or a corrupt record,
 * but they must still return a usable string rather than NULL. */
static void test_status_out_of_range_falls_back(void) {
    TEST_ASSERT_EQUAL_STRING("INTERNAL",
                             aegis_status_code((aegis_status_t)999));
    TEST_ASSERT_NOT_NULL(aegis_status_message((aegis_status_t)999));
}

static void test_memory_type_roundtrip(void) {
    const char *names[] = {"working", "episodic", "semantic"};
    for (size_t i = 0; i < 3; i++) {
        MemoryType t;
        TEST_ASSERT_EQUAL_INT(0, memory_type_from_string(names[i], &t));
        TEST_ASSERT_EQUAL_STRING(names[i], memory_type_to_string(t));
    }
}

static void test_memory_type_rejects_unknown(void) {
    MemoryType t = MEM_EPISODIC;
    TEST_ASSERT_EQUAL_INT(-1, memory_type_from_string("", &t));
    TEST_ASSERT_EQUAL_INT(-1,
                          memory_type_from_string("Episodic", &t)); /* case */
    TEST_ASSERT_EQUAL_INT(-1, memory_type_from_string("procedural", &t));
    /* A rejected parse must not have written through the out pointer. */
    TEST_ASSERT_EQUAL_INT(MEM_EPISODIC, t);
}

/* A missing "type" field arrives here as NULL; it must be rejected, not
 * dereferenced. */
static void test_memory_type_rejects_null(void) {
    MemoryType t;
    TEST_ASSERT_EQUAL_INT(-1, memory_type_from_string(NULL, &t));
}

static void test_memory_type_to_string_out_of_range(void) {
    TEST_ASSERT_EQUAL_STRING("unknown", memory_type_to_string((MemoryType)42));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_status_code_strings);
    RUN_TEST(test_status_table_covers_enum);
    RUN_TEST(test_status_codes_are_distinct);
    RUN_TEST(test_status_messages_present);
    RUN_TEST(test_status_out_of_range_falls_back);
    RUN_TEST(test_memory_type_roundtrip);
    RUN_TEST(test_memory_type_rejects_unknown);
    RUN_TEST(test_memory_type_rejects_null);
    RUN_TEST(test_memory_type_to_string_out_of_range);
    return UNITY_END();
}