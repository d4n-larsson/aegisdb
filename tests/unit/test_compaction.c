/* Tests for log compaction: it must drop tombstones/superseded versions while
 * preserving live records, produce a log that recovers cleanly, and stay
 * correct when records are written concurrently with the compaction pass. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/compaction.h"
#include "aegisdb/db.h"
#include "aegisdb/query_engine.h"
#include "unity.h"

static AegisDB g_db;
static char g_dir[256];

void setUp(void) {
    snprintf(g_dir, sizeof(g_dir), "/tmp/aegis_compact_%d_%ld", (int)getpid(),
             (long)random());
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    (void)!system(cmd);

    Config cfg;
    config_defaults(&cfg);
    strncpy(cfg.data_dir, g_dir, sizeof(cfg.data_dir) - 1);
    TEST_ASSERT_EQUAL_INT(0, db_open(&g_db, &cfg));
}

void tearDown(void) {
    db_close(&g_db);
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    (void)!system(cmd);
}

static uint64_t insert_ep(AegisDB *db, const char *data) {
    MemoryRecord in;
    record_init(&in);
    in.type = MEM_EPISODIC;
    in.data = (void *)data;
    in.data_len = strlen(data);
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(db, &in, NULL, 0, &out));
    uint64_t id = out.id;
    record_free(&out);
    return id;
}

static void assert_data(AegisDB *db, uint64_t id, const char *expect) {
    MemoryRecord r;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_get(db, id, NULL, &r));
    TEST_ASSERT_EQUAL_size_t(strlen(expect), r.data_len);
    TEST_ASSERT_EQUAL_MEMORY(expect, r.data, r.data_len);
    record_free(&r);
}

/* Live records survive compaction; deleted ones are dropped; the log shrinks. */
static void test_compaction_preserves_live_drops_deleted(void) {
    uint64_t keep[3], drop[2];
    for (int i = 0; i < 3; i++) {
        char b[16];
        snprintf(b, sizeof(b), "keep-%d", i);
        keep[i] = insert_ep(&g_db, b);
    }
    for (int i = 0; i < 2; i++) {
        char b[16];
        snprintf(b, sizeof(b), "drop-%d", i);
        drop[i] = insert_ep(&g_db, b);
        TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_delete(&g_db, drop[i], NULL));
    }

    uint64_t before = (uint64_t)g_db.log.size;
    TEST_ASSERT_EQUAL_INT(0, compaction_run_once(&g_db));
    uint64_t after = (uint64_t)g_db.log.size;
    TEST_ASSERT_TRUE(after < before); /* tombstones + dead frames reclaimed */

    for (int i = 0; i < 3; i++) {
        char b[16];
        snprintf(b, sizeof(b), "keep-%d", i);
        assert_data(&g_db, keep[i], b);
    }
    MemoryRecord r;
    for (int i = 0; i < 2; i++)
        TEST_ASSERT_EQUAL_INT(AEGIS_ERR_NOT_FOUND,
                              qe_get(&g_db, drop[i], NULL, &r));
}

/* The compacted log must reload correctly through normal recovery. */
static void test_compacted_log_recovers(void) {
    uint64_t a = insert_ep(&g_db, "alpha");
    uint64_t b = insert_ep(&g_db, "beta");
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_delete(&g_db, a, NULL));
    TEST_ASSERT_EQUAL_INT(0, compaction_run_once(&g_db));

    /* Reopen the database: recovery rebuilds indexes from the compacted log. */
    db_close(&g_db);
    Config cfg;
    config_defaults(&cfg);
    strncpy(cfg.data_dir, g_dir, sizeof(cfg.data_dir) - 1);
    TEST_ASSERT_EQUAL_INT(0, db_open(&g_db, &cfg));

    assert_data(&g_db, b, "beta");
    MemoryRecord r;
    TEST_ASSERT_EQUAL_INT(AEGIS_ERR_NOT_FOUND, qe_get(&g_db, a, NULL, &r));
}

/* Concurrency: a writer inserts records while compaction runs repeatedly. Every
 * record acknowledged by the writer must survive — exercising the unlocked copy
 * racing the tail-drain. */
#define NWRITES 400
static uint64_t g_ids[NWRITES];

static void *writer(void *arg) {
    AegisDB *db = arg;
    for (int i = 0; i < NWRITES; i++) {
        char b[24];
        snprintf(b, sizeof(b), "concurrent-%d", i);
        g_ids[i] = insert_ep(db, b);
    }
    return NULL;
}

static void test_compaction_concurrent_with_writes(void) {
    pthread_t th;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, writer, &g_db));
    /* Hammer compaction while the writer races it. */
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_INT(0, compaction_run_once(&g_db));
        usleep(200);
    }
    pthread_join(th, NULL);
    /* One final pass to fold in everything written after the last compaction. */
    TEST_ASSERT_EQUAL_INT(0, compaction_run_once(&g_db));

    for (int i = 0; i < NWRITES; i++) {
        char b[24];
        snprintf(b, sizeof(b), "concurrent-%d", i);
        assert_data(&g_db, g_ids[i], b);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_compaction_preserves_live_drops_deleted);
    RUN_TEST(test_compacted_log_recovers);
    RUN_TEST(test_compaction_concurrent_with_writes);
    return UNITY_END();
}