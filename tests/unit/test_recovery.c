/* Tests for checkpoint-based recovery: the index checkpoint lets recovery trust
 * the covered prefix and replay only the tail, with a clean fallback to a full
 * scan when the checkpoint is absent, stale, or corrupt. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/db.h"
#include "aegisdb/query_engine.h"
#include "unity.h"

static char g_dir[256];

void setUp(void) {
    snprintf(g_dir, sizeof(g_dir), "/tmp/aegis_recov_%d_%ld", (int)getpid(),
             (long)random());
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    (void)!system(cmd);
}

void tearDown(void) {
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    (void)!system(cmd);
}

static void open_db(AegisDB *db) {
    Config cfg;
    config_defaults(&cfg);
    strncpy(cfg.data_dir, g_dir, sizeof(cfg.data_dir) - 1);
    cfg.checkpoint_sec = 0; /* drive checkpoints explicitly in tests */
    TEST_ASSERT_EQUAL_INT(0, db_open(db, &cfg));
}

/* Open with a small embedding dim and a low ANN threshold so a handful of
 * vectors crosses it and exercises the HNSW build/persist path. */
static void open_db_ann(AegisDB *db, size_t threshold) {
    Config cfg;
    config_defaults(&cfg);
    strncpy(cfg.data_dir, g_dir, sizeof(cfg.data_dir) - 1);
    cfg.checkpoint_sec = 0;
    cfg.embedding_dimensions = 4;
    cfg.ann_threshold = threshold;
    cfg.ann_ef_search = 64;
    TEST_ASSERT_EQUAL_INT(0, db_open(db, &cfg));
}

/* id i (1-based) -> vector (1, i*0.25, 0, 0): distinct, monotonic directions. */
static void vec_for(unsigned i, float out[4]) {
    out[0] = 1.0f;
    out[1] = (float)i * 0.25f;
    out[2] = out[3] = 0.0f;
}

static uint64_t insert_vec(AegisDB *db, const float *emb) {
    MemoryRecord in;
    record_init(&in);
    in.type = MEM_EPISODIC;
    in.data = (void *)"v";
    in.data_len = 1;
    in.embedding = (float *)emb;
    in.embedding_dim = 4;
    in.vec_count = 1;
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(db, &in, NULL, 0, &out));
    uint64_t id = out.id;
    in.data = NULL; /* borrowed */
    in.data_len = 0;
    in.embedding = NULL; /* borrowed */
    in.embedding_dim = 0;
    record_free(&in);
    record_free(&out);
    return id;
}

static uint64_t search_top(AegisDB *db, const float *emb) {
    SearchParams p = {0};
    p.embedding = (float *)emb;
    p.embedding_dim = 4;
    p.top_k = 1;
    MemoryRecord *recs = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(db, &p, &recs, &n));
    uint64_t id = n ? recs[0].id : 0;
    for (size_t i = 0; i < n; i++) record_free(&recs[i]);
    free(recs);
    return id;
}

static uint64_t insert_ep(AegisDB *db, const char *data) {
    MemoryRecord in;
    record_init(&in);
    in.type = MEM_EPISODIC;
    const char *tags[] = {"r"};
    record_set_tags(&in, tags, 1);
    in.data = (void *)data;
    in.data_len = strlen(data);
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(db, &in, NULL, 0, &out));
    uint64_t id = out.id;
    in.data = NULL; /* borrowed string literal; don't free it via record_free */
    in.data_len = 0;
    record_free(&in); /* frees the tags allocated by record_set_tags */
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

/* Records written AFTER the last checkpoint must be replayed from the log tail.
 * Simulates a crash by reopening a second handle on the fsync'd files without a
 * clean close (so no fresh checkpoint covers the tail). */
static void test_recovery_replays_tail_after_checkpoint(void) {
    AegisDB db1;
    open_db(&db1);
    uint64_t a = insert_ep(&db1, "alpha");
    uint64_t b = insert_ep(&db1, "bravo");
    TEST_ASSERT_EQUAL_INT(0, db_checkpoint(&db1)); /* covers alpha, bravo */
    /* These land in the log tail, beyond the checkpoint's covered offset. */
    uint64_t c = insert_ep(&db1, "charlie");
    uint64_t d = insert_ep(&db1, "delta");
    log_fsync(&db1.log);

    AegisDB db2;
    open_db(&db2); /* recovery: load checkpoint + replay tail */
    assert_data(&db2, a, "alpha");
    assert_data(&db2, b, "bravo");
    assert_data(&db2, c, "charlie"); /* recovered from the tail */
    assert_data(&db2, d, "delta");
    /* next_id must clear the tail's ids so a new insert does not collide. */
    uint64_t e = insert_ep(&db2, "echo");
    TEST_ASSERT_TRUE(e > a && e > b && e > c && e > d);
    db_close(&db2);

    db_close(&db1);
}

/* A tag written only in the tail must reach the secondary indexes too (recovery
 * pass 2 rebuilds them from every live record, checkpointed or replayed). */
static void test_recovery_rebuilds_secondary_from_tail(void) {
    AegisDB db1;
    open_db(&db1);
    insert_ep(&db1, "first");
    TEST_ASSERT_EQUAL_INT(0, db_checkpoint(&db1));
    uint64_t t = insert_ep(&db1, "tail-tagged");
    log_fsync(&db1.log);

    AegisDB db2;
    open_db(&db2);
    SearchParams p = {0};
    const char *tags[] = {"r"};
    p.tags = tags;
    p.tag_count = 1;
    p.match_all = 1;
    p.top_k = 10;
    MemoryRecord *recs = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(&db2, &p, &recs, &n));
    TEST_ASSERT_EQUAL_size_t(2, n); /* both the checkpointed and tail record */
    int found_tail = 0;
    for (size_t i = 0; i < n; i++) {
        if (recs[i].id == t) found_tail = 1;
        record_free(&recs[i]);
    }
    free(recs);
    TEST_ASSERT_TRUE(found_tail);
    db_close(&db2);
    db_close(&db1);
}

/* A corrupt checkpoint is rejected; recovery falls back to a full log scan and
 * still loads every record. */
static void test_recovery_falls_back_on_corrupt_checkpoint(void) {
    AegisDB db1;
    open_db(&db1);
    uint64_t a = insert_ep(&db1, "one");
    uint64_t b = insert_ep(&db1, "two");
    db_close(&db1); /* writes a checkpoint covering both */

    /* Corrupt the checkpoint body. */
    char idx[320];
    snprintf(idx, sizeof(idx), "%s/memory.index", g_dir);
    int fd = open(idx, O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);
    uint8_t byte = 0;
    TEST_ASSERT_EQUAL_INT(1, pread(fd, &byte, 1, 40)); /* inside an entry */
    byte ^= 0xFF;
    TEST_ASSERT_EQUAL_INT(1, pwrite(fd, &byte, 1, 40));
    close(fd);

    AegisDB db2;
    open_db(&db2); /* checkpoint rejected -> full scan */
    assert_data(&db2, a, "one");
    assert_data(&db2, b, "two");
    db_close(&db2);
}

/* next_id must not regress (and reuse an id) when a tail-truncating crash
 * leaves the log shorter than the checkpoint's covered offset. The metadata
 * high-water next_id floors it. */
static void test_recovery_next_id_does_not_regress(void) {
    AegisDB db1;
    open_db(&db1);
    insert_ep(&db1, "one");                       /* id 1 */
    uint64_t after_two = 0;
    insert_ep(&db1, "two");                        /* id 2 */
    after_two = (uint64_t)db1.log.size;            /* boundary before id 3 */
    insert_ep(&db1, "three");                      /* id 3 */
    db_close(&db1);  /* checkpoint covers all 3 (covered > after_two); meta next_id=4 */

    /* Simulate a crash that truncated the log below the checkpoint's covered
     * offset: drop id 3's frame. The checkpoint is now stale -> rejected ->
     * full scan finds only ids 1,2. */
    char log[320];
    snprintf(log, sizeof(log), "%s/memory.log", g_dir);
    TEST_ASSERT_EQUAL_INT(0, truncate(log, (off_t)after_two));

    AegisDB db2;
    open_db(&db2);
    /* id 3 is gone... */
    MemoryRecord r;
    TEST_ASSERT_EQUAL_INT(AEGIS_ERR_NOT_FOUND, qe_get(&db2, 3, NULL, &r));
    /* ...but a new insert must NOT reuse id 3 — next_id floored by metadata. */
    uint64_t fresh = insert_ep(&db2, "four");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(4, fresh);
    db_close(&db2);
}

/* A clean close persists the HNSW graph; recovery loads it and the dense array
 * it rebuilds answers semantic search correctly. */
static void test_recovery_semantic_checkpoint(void) {
    const unsigned N = 20;
    float v[21][4];
    uint64_t ids[21];
    AegisDB db1;
    open_db_ann(&db1, 8); /* HNSW once >8 vectors */
    for (unsigned i = 1; i <= N; i++) {
        vec_for(i, v[i]);
        ids[i] = insert_vec(&db1, v[i]);
    }
    /* The graph build is deferred to the maintenance thread on a live server;
     * drive it directly here (no maintenance thread in the unit test). */
    TEST_ASSERT_EQUAL_INT(1, db_semantic_build_step(&db1));
    db_close(&db1); /* writes memory.sem (graph exists) */

    char sem[320];
    snprintf(sem, sizeof(sem), "%s/memory.sem", g_dir);
    TEST_ASSERT_EQUAL_INT(0, access(sem, F_OK)); /* checkpoint was written */

    AegisDB db2;
    open_db_ann(&db2, 8); /* recovery loads the graph + rebuilds the dense array */
    TEST_ASSERT_EQUAL_UINT64(ids[5], search_top(&db2, v[5]));
    TEST_ASSERT_EQUAL_UINT64(ids[N], search_top(&db2, v[N]));
    db_close(&db2);
}

/* Vectors written after the checkpoint are replayed from the log tail onto the
 * loaded graph. */
static void test_recovery_semantic_tail(void) {
    float v[17][4];
    uint64_t ids[17];
    AegisDB db1;
    open_db_ann(&db1, 8);
    for (unsigned i = 1; i <= 12; i++) { vec_for(i, v[i]); ids[i] = insert_vec(&db1, v[i]); }
    TEST_ASSERT_EQUAL_INT(1, db_semantic_build_step(&db1)); /* build the graph */
    TEST_ASSERT_EQUAL_INT(0, db_checkpoint(&db1)); /* covers 1..12 */
    for (unsigned i = 13; i <= 16; i++) { vec_for(i, v[i]); ids[i] = insert_vec(&db1, v[i]); }
    log_fsync(&db1.log);

    AegisDB db2;
    open_db_ann(&db2, 8); /* load checkpoint + replay tail 13..16 */
    TEST_ASSERT_EQUAL_UINT64(ids[15], search_top(&db2, v[15])); /* a tail vector */
    TEST_ASSERT_EQUAL_UINT64(ids[3], search_top(&db2, v[3]));   /* a checkpointed one */
    db_close(&db2);
    db_close(&db1);
}

/* An id deleted in the tail is in the loaded graph but must be reconciled out:
 * pass 2 skips deleted records, so recovery drops ids the hash no longer holds. */
static void test_recovery_semantic_tail_delete(void) {
    float v[13][4];
    uint64_t ids[13];
    AegisDB db1;
    open_db_ann(&db1, 8);
    for (unsigned i = 1; i <= 12; i++) { vec_for(i, v[i]); ids[i] = insert_vec(&db1, v[i]); }
    TEST_ASSERT_EQUAL_INT(1, db_semantic_build_step(&db1)); /* build the graph */
    TEST_ASSERT_EQUAL_INT(0, db_checkpoint(&db1)); /* graph includes id 5 */
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_delete(&db1, ids[5], NULL)); /* tail delete */
    log_fsync(&db1.log);

    AegisDB db2;
    open_db_ann(&db2, 8);
    MemoryRecord r;
    TEST_ASSERT_EQUAL_INT(AEGIS_ERR_NOT_FOUND, qe_get(&db2, ids[5], NULL, &r));
    TEST_ASSERT_NOT_EQUAL(ids[5], search_top(&db2, v[5])); /* gone from results */
    db_close(&db2);
    db_close(&db1);
}

/* A corrupt semantic checkpoint is rejected; recovery rebuilds the graph from
 * the log and search still works. */
static void test_recovery_semantic_corrupt_fallback(void) {
    float v[13][4];
    uint64_t ids[13];
    AegisDB db1;
    open_db_ann(&db1, 8);
    for (unsigned i = 1; i <= 12; i++) { vec_for(i, v[i]); ids[i] = insert_vec(&db1, v[i]); }
    TEST_ASSERT_EQUAL_INT(1, db_semantic_build_step(&db1)); /* build the graph */
    db_close(&db1);

    char sem[320];
    snprintf(sem, sizeof(sem), "%s/memory.sem", g_dir);
    int fd = open(sem, O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);
    uint8_t byte = 0;
    TEST_ASSERT_EQUAL_INT(1, pread(fd, &byte, 1, 48));
    byte ^= 0xFF;
    TEST_ASSERT_EQUAL_INT(1, pwrite(fd, &byte, 1, 48));
    close(fd);

    AegisDB db2;
    open_db_ann(&db2, 8); /* checkpoint rejected -> full rebuild */
    TEST_ASSERT_EQUAL_UINT64(ids[7], search_top(&db2, v[7]));
    db_close(&db2);
}

/* In sync durability the per-write fsync now runs after the index lock is
 * released; the write path must still leave the log flushed before returning
 * (ack-after-durable). In interval durability the insert defers the flush to
 * the maintenance thread, so a flush stays pending. */
static void test_sync_durability_flushes_per_write(void) {
    Config cfg;
    config_defaults(&cfg);
    strncpy(cfg.data_dir, g_dir, sizeof(cfg.data_dir) - 1);
    cfg.checkpoint_sec = 0;
    cfg.durability = AEGIS_DURABILITY_SYNC;
    AegisDB db;
    TEST_ASSERT_EQUAL_INT(0, db_open(&db, &cfg));
    insert_ep(&db, "durable");
    TEST_ASSERT_FALSE(log_flush_pending(&db.log)); /* fsync ran off the lock */
    db_close(&db);

    char dir2[300];
    snprintf(dir2, sizeof(dir2), "%s_iv", g_dir);
    char rm[340];
    snprintf(rm, sizeof(rm), "rm -rf '%s'", dir2);
    (void)!system(rm);
    config_defaults(&cfg);
    strncpy(cfg.data_dir, dir2, sizeof(cfg.data_dir) - 1);
    cfg.checkpoint_sec = 0; /* default durability = interval */
    AegisDB db2;
    TEST_ASSERT_EQUAL_INT(0, db_open(&db2, &cfg));
    insert_ep(&db2, "deferred");
    TEST_ASSERT_TRUE(log_flush_pending(&db2.log)); /* interval: flush deferred */
    db_close(&db2);
    (void)!system(rm);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_sync_durability_flushes_per_write);
    RUN_TEST(test_recovery_replays_tail_after_checkpoint);
    RUN_TEST(test_recovery_rebuilds_secondary_from_tail);
    RUN_TEST(test_recovery_falls_back_on_corrupt_checkpoint);
    RUN_TEST(test_recovery_next_id_does_not_regress);
    RUN_TEST(test_recovery_semantic_checkpoint);
    RUN_TEST(test_recovery_semantic_tail);
    RUN_TEST(test_recovery_semantic_tail_delete);
    RUN_TEST(test_recovery_semantic_corrupt_fallback);
    return UNITY_END();
}