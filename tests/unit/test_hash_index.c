/* Unit tests for the id -> log-location hash index, including snapshots. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "aegisdb/hash_index.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void tmp_path(char *buf, size_t n, const char *tag) {
    snprintf(buf, n, "/tmp/aegis_test_%s_%d.bin", tag, (int)getpid());
}

static void test_put_get(void) {
    HashIndex *h = hash_index_create();
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(0, hash_index_put(h, 1, 0, 100, 1, 0));
    TEST_ASSERT_EQUAL_INT(0, hash_index_put(h, 2, 108, 50, 2, 0));

    const HashEntry *e = hash_index_get(h, 1);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT64(0, e->offset);
    TEST_ASSERT_EQUAL_UINT32(100, e->length);
    TEST_ASSERT_EQUAL_UINT8(1, e->type);

    e = hash_index_get(h, 2);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT64(108, e->offset);

    TEST_ASSERT_NULL(hash_index_get(h, 999));
    TEST_ASSERT_EQUAL_size_t(2, hash_index_count(h));
    hash_index_free(h);
}

static void test_update_overwrites(void) {
    HashIndex *h = hash_index_create();
    hash_index_put(h, 5, 0, 10, 1, 0);
    hash_index_put(h, 5, 200, 20, 2, 0); /* same id -> latest location wins */
    const HashEntry *e = hash_index_get(h, 5);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT64(200, e->offset);
    TEST_ASSERT_EQUAL_UINT32(20, e->length);
    TEST_ASSERT_EQUAL_size_t(1, hash_index_count(h));
    hash_index_free(h);
}

/* Forces several rehash/grow cycles to exercise open-addressing probing. */
static void test_many_entries(void) {
    HashIndex *h = hash_index_create();
    for (uint64_t i = 1; i <= 1000; i++)
        TEST_ASSERT_EQUAL_INT(0, hash_index_put(h, i, i * 8, 4, 1, 0));
    TEST_ASSERT_EQUAL_size_t(1000, hash_index_count(h));
    for (uint64_t i = 1; i <= 1000; i++) {
        const HashEntry *e = hash_index_get(h, i);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_UINT64(i * 8, e->offset);
    }
    hash_index_free(h);
}

static void test_save_load_roundtrip(void) {
    char path[256];
    tmp_path(path, sizeof(path), "hashidx");

    HashIndex *h = hash_index_create();
    hash_index_put(h, 10, 1000, 5, 1, 0);
    hash_index_put(h, 20, 2000, 6, 2, 1); /* tombstoned entry */
    TEST_ASSERT_EQUAL_INT(0, hash_index_save(h, path));
    hash_index_free(h);

    HashIndex *h2 = hash_index_create();
    TEST_ASSERT_EQUAL_INT(0, hash_index_load(h2, path));
    const HashEntry *e = hash_index_get(h2, 10);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT64(1000, e->offset);
    TEST_ASSERT_EQUAL_UINT32(5, e->length);
    hash_index_free(h2);
    remove(path);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_put_get);
    RUN_TEST(test_update_overwrites);
    RUN_TEST(test_many_entries);
    RUN_TEST(test_save_load_roundtrip);
    return UNITY_END();
}