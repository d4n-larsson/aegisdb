/* Unit tests for per-tenant usage accounting + token-bucket rate limiting. */
#include <string.h>

#include "aegisdb/tenant.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Usage adjusts by signed deltas and saturates at 0 rather than underflowing. */
static void test_usage_adjust_and_saturate(void) {
    TenantTable *t = tenant_table_create();
    TEST_ASSERT_NOT_NULL(t);

    tenant_usage_adjust(t, "acme", 1, 100);
    tenant_usage_adjust(t, "acme", 1, 50);
    tenant_usage_adjust(t, "beta", 3, 300);

    size_t n = 0;
    TenantUsage *u = tenant_usage_snapshot(t, &n);
    TEST_ASSERT_EQUAL_size_t(2, n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(u[i].ns, "acme") == 0) {
            TEST_ASSERT_EQUAL_UINT64(2, u[i].records);
            TEST_ASSERT_EQUAL_UINT64(150, u[i].bytes);
        } else {
            TEST_ASSERT_EQUAL_UINT64(3, u[i].records);
            TEST_ASSERT_EQUAL_UINT64(300, u[i].bytes);
        }
    }
    tenant_usage_free(u, n);

    /* over-subtract saturates at 0, never wraps */
    tenant_usage_adjust(t, "acme", -10, -10000);
    u = tenant_usage_snapshot(t, &n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(u[i].ns, "acme") == 0) {
            TEST_ASSERT_EQUAL_UINT64(0, u[i].records);
            TEST_ASSERT_EQUAL_UINT64(0, u[i].bytes);
        }
    tenant_usage_free(u, n);
    tenant_table_free(t);
}

/* would_exceed respects each cap, treats 0 as unlimited, and ignores non-positive
 * additions (a delete never trips a quota). */
static void test_would_exceed(void) {
    TenantTable *t = tenant_table_create();
    tenant_usage_adjust(t, "acme", 2, 200);

    /* record cap: at 2, adding 1 with max 2 exceeds; max 0 = unlimited */
    TEST_ASSERT_EQUAL_INT(-1, tenant_usage_would_exceed(t, "acme", 1, 0, 2, 0));
    TEST_ASSERT_EQUAL_INT(0, tenant_usage_would_exceed(t, "acme", 1, 0, 3, 0));
    TEST_ASSERT_EQUAL_INT(0, tenant_usage_would_exceed(t, "acme", 1, 0, 0, 0));
    /* byte cap */
    TEST_ASSERT_EQUAL_INT(-1, tenant_usage_would_exceed(t, "acme", 0, 50, 0, 200));
    TEST_ASSERT_EQUAL_INT(0, tenant_usage_would_exceed(t, "acme", 0, 50, 0, 300));
    /* a shrink (non-positive add) never exceeds */
    TEST_ASSERT_EQUAL_INT(0, tenant_usage_would_exceed(t, "acme", -1, -50, 1, 1));
    /* an unknown tenant starts at 0 usage */
    TEST_ASSERT_EQUAL_INT(0, tenant_usage_would_exceed(t, "new", 1, 100, 5, 500));

    tenant_table_free(t);
}

/* Token bucket: a full burst is spendable immediately, then requests are denied
 * until tokens refill at the configured rate. qps<=0 disables the limit. */
static void test_rate_bucket(void) {
    TenantTable *t = tenant_table_create();
    uint64_t now = 1000000; /* 1s in micros */
    const double qps = 10, burst = 10;

    int allowed = 0;
    for (int i = 0; i < 10; i++)
        allowed += tenant_rate_allow(t, "acme", qps, burst, now);
    TEST_ASSERT_EQUAL_INT(10, allowed);              /* full bucket spent */
    TEST_ASSERT_EQUAL_INT(0, tenant_rate_allow(t, "acme", qps, burst, now)); /* empty */

    /* +0.5s at 10 qps refills ~5 tokens */
    now += 500000;
    allowed = 0;
    for (int i = 0; i < 10; i++)
        allowed += tenant_rate_allow(t, "acme", qps, burst, now);
    TEST_ASSERT_TRUE(allowed >= 4 && allowed <= 6);

    /* qps<=0 => unlimited; a different tenant has its own bucket */
    for (int i = 0; i < 100; i++)
        TEST_ASSERT_EQUAL_INT(1, tenant_rate_allow(t, "acme", 0, 0, now));
    TEST_ASSERT_EQUAL_INT(1, tenant_rate_allow(t, "beta", qps, burst, now));

    tenant_table_free(t);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_usage_adjust_and_saturate);
    RUN_TEST(test_would_exceed);
    RUN_TEST(test_rate_bucket);
    return UNITY_END();
}