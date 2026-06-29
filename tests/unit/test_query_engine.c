/* End-to-end tests for the query engine over a real temporary database.
 * Covers US1 (persist/get), US2 (update/search), US3 (semantic search),
 * US4 (working memory + promote) and US5 (relate/traverse). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/db.h"
#include "aegisdb/query_engine.h"
#include "unity.h"

static AegisDB g_db;
static char g_dir[256];

void setUp(void) {
    snprintf(g_dir, sizeof(g_dir), "/tmp/aegis_qe_%d_%ld", (int)getpid(),
             (long)random());
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    (void)!system(cmd);

    Config cfg;
    config_defaults(&cfg);
    strncpy(cfg.data_dir, g_dir, sizeof(cfg.data_dir) - 1);
    cfg.embedding_dimensions = 3; /* keep test vectors small */
    TEST_ASSERT_EQUAL_INT(0, db_open(&g_db, &cfg));
}

void tearDown(void) {
    db_close(&g_db);
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    (void)!system(cmd);
}

/* Build a minimal episodic insert input. Caller frees out via record_free. */
static MemoryRecord make_input(MemoryType type, const char *data) {
    MemoryRecord in;
    record_init(&in);
    in.type = type;
    in.data = (void *)data; /* borrowed; qe_insert copies */
    in.data_len = strlen(data);
    return in;
}

static void test_insert_get_episodic(void) {
    MemoryRecord in = make_input(MEM_EPISODIC, "User likes coffee");
    const char *tags[] = {"user", "preference"};
    record_set_tags(&in, tags, 2);
    in.importance = 0.7f;

    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(&g_db, &in, NULL, 0, &out));
    TEST_ASSERT_GREATER_THAN_UINT64(0, out.id);
    TEST_ASSERT_EQUAL_INT(MEM_EPISODIC, out.type);
    TEST_ASSERT_EQUAL_UINT64(out.created, out.updated); /* episodic invariant */
    uint64_t id = out.id;
    record_free(&out);

    MemoryRecord got;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_get(&g_db, id, NULL, &got));
    TEST_ASSERT_EQUAL_size_t(strlen("User likes coffee"), got.data_len);
    TEST_ASSERT_EQUAL_MEMORY("User likes coffee", got.data, got.data_len);
    TEST_ASSERT_EQUAL_size_t(2, got.tag_count);
    record_free(&got);

    /* tags array allocated by record_set_tags must be released. */
    free(in.tags[0]);
    free(in.tags[1]);
    free(in.tags);
}

static void test_get_not_found(void) {
    MemoryRecord got;
    TEST_ASSERT_EQUAL_INT(AEGIS_ERR_NOT_FOUND, qe_get(&g_db, 123456, NULL, &got));
}

static void test_episodic_update_rejected(void) {
    MemoryRecord in = make_input(MEM_EPISODIC, "immutable");
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(&g_db, &in, NULL, 0, &out));
    uint64_t id = out.id;
    record_free(&out);

    UpdatePatch patch = {0};
    patch.has_data = 1;
    patch.data = "changed";
    patch.data_len = 7;
    MemoryRecord upd;
    TEST_ASSERT_EQUAL_INT(AEGIS_ERR_IMMUTABLE,
                          qe_update(&g_db, id, &patch, NULL, &upd));
}

static void test_semantic_update(void) {
    MemoryRecord in = make_input(MEM_SEMANTIC, "sky is blue");
    in.confidence = 1.0f;
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(&g_db, &in, NULL, 0, &out));
    uint64_t id = out.id;
    record_free(&out);

    UpdatePatch patch = {0};
    patch.has_data = 1;
    patch.data = "sky is azure";
    patch.data_len = 12;
    patch.has_confidence = 1;
    patch.confidence = 0.9f;
    MemoryRecord upd;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_update(&g_db, id, &patch, NULL, &upd));
    TEST_ASSERT_EQUAL_MEMORY("sky is azure", upd.data, upd.data_len);
    TEST_ASSERT_EQUAL_FLOAT(0.9f, upd.confidence);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(upd.created, upd.updated);
    record_free(&upd);
}

static void test_search_by_time_and_tags(void) {
    const char *tagset[] = {"alpha"};
    for (int i = 0; i < 3; i++) {
        MemoryRecord in = make_input(MEM_EPISODIC, "rec");
        if (i == 0) record_set_tags(&in, tagset, 1);
        MemoryRecord out;
        TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(&g_db, &in, NULL, 0, &out));
        record_free(&out);
        if (i == 0) {
            free(in.tags[0]);
            free(in.tags);
        }
    }

    /* Time range covering everything. */
    SearchParams p = {0};
    p.has_time = 1;
    p.start_time = 0;
    p.end_time = (uint64_t)-1;
    p.top_k = 100;
    MemoryRecord *recs = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(&g_db, &p, &recs, &n));
    TEST_ASSERT_EQUAL_size_t(3, n);
    for (size_t i = 0; i < n; i++) record_free(&recs[i]);
    free(recs);

    /* Tag filter -> only the one tagged record. */
    SearchParams pt = {0};
    const char *q[] = {"alpha"};
    pt.tags = q;
    pt.tag_count = 1;
    pt.match_all = 1;
    pt.top_k = 100;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(&g_db, &pt, &recs, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    record_free(&recs[0]);
    free(recs);
}

static void test_semantic_search(void) {
    float v1[] = {1.0f, 0.0f, 0.0f};
    float v2[] = {0.0f, 1.0f, 0.0f};
    MemoryRecord a = make_input(MEM_SEMANTIC, "a");
    a.embedding = v1;
    a.embedding_dim = 3;
    MemoryRecord b = make_input(MEM_SEMANTIC, "b");
    b.embedding = v2;
    b.embedding_dim = 3;
    MemoryRecord oa, ob;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(&g_db, &a, NULL, 0, &oa));
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(&g_db, &b, NULL, 0, &ob));
    uint64_t id_a = oa.id;
    record_free(&oa);
    record_free(&ob);

    SearchParams p = {0};
    float query[] = {0.95f, 0.05f, 0.0f};
    p.embedding = query;
    p.embedding_dim = 3;
    p.top_k = 1;
    MemoryRecord *recs = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(&g_db, &p, &recs, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(id_a, recs[0].id); /* nearest to query */
    record_free(&recs[0]);
    free(recs);
}

static void test_working_memory_promote(void) {
    MemoryRecord in = make_input(MEM_WORKING, "scratch note");
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK,
                          qe_insert(&g_db, &in, "sess-1", 60000, &out));
    uint64_t wid = out.id;
    record_free(&out);

    MemoryRecord promoted;
    TEST_ASSERT_EQUAL_INT(
        AEGIS_OK,
        qe_promote(&g_db, "sess-1", wid, MEM_EPISODIC, NULL, &promoted));
    TEST_ASSERT_EQUAL_INT(MEM_EPISODIC, promoted.type);
    uint64_t pid = promoted.id;
    record_free(&promoted);

    /* Promoted record is now durably retrievable by its persisted id. */
    MemoryRecord got;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_get(&g_db, pid, NULL, &got));
    TEST_ASSERT_EQUAL_MEMORY("scratch note", got.data, got.data_len);
    record_free(&got);
}

static void test_relate_and_traverse(void) {
    MemoryRecord ia = make_input(MEM_EPISODIC, "A");
    MemoryRecord ib = make_input(MEM_SEMANTIC, "B");
    MemoryRecord oa, ob;
    qe_insert(&g_db, &ia, NULL, 0, &oa);
    qe_insert(&g_db, &ib, NULL, 0, &ob);
    uint64_t from = oa.id, to = ob.id;
    record_free(&oa);
    record_free(&ob);

    TEST_ASSERT_EQUAL_INT(AEGIS_OK,
                          qe_relate(&g_db, from, to, "derived_from", NULL));

    /* Relating from a nonexistent id must fail with NOT_FOUND. */
    TEST_ASSERT_EQUAL_INT(AEGIS_ERR_NOT_FOUND,
                          qe_relate(&g_db, 999999, to, "x", NULL));

    MemoryRecord *recs = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK,
                          qe_traverse(&g_db, from, 2, NULL, &recs, &n));
    /* Traversal reaches the related target. */
    int saw_to = 0;
    for (size_t i = 0; i < n; i++) {
        if (recs[i].id == to) saw_to = 1;
        record_free(&recs[i]);
    }
    free(recs);
    TEST_ASSERT_TRUE(saw_to);
}

static void test_agent_namespace_filter(void) {
    MemoryRecord in = make_input(MEM_EPISODIC, "owned");
    in.agent_id = strdup("agent-A");
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(&g_db, &in, NULL, 0, &out));
    uint64_t id = out.id;
    free(in.agent_id);
    record_free(&out);

    MemoryRecord got;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_get(&g_db, id, "agent-A", &got));
    record_free(&got);
    /* Wrong namespace must not see the record. */
    TEST_ASSERT_EQUAL_INT(AEGIS_ERR_NOT_FOUND,
                          qe_get(&g_db, id, "agent-B", &got));
}

/* Writes scoped to a namespace must treat another tenant's record as missing.
 * Critically, an update from the wrong namespace returns NOT_FOUND, not the
 * type-specific IMMUTABLE, so existence/type cannot leak across tenants. */
static void test_ns_scoped_writes(void) {
    MemoryRecord sem = make_input(MEM_SEMANTIC, "A's fact");
    sem.agent_id = strdup("agent-A");
    MemoryRecord os;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(&g_db, &sem, NULL, 0, &os));
    uint64_t sid = os.id;
    free(sem.agent_id);
    record_free(&os);

    UpdatePatch patch = {0};
    patch.has_data = 1;
    patch.data = "edited";
    patch.data_len = 6;
    MemoryRecord upd;

    /* wrong tenant: indistinguishable from missing */
    TEST_ASSERT_EQUAL_INT(AEGIS_ERR_NOT_FOUND,
                          qe_update(&g_db, sid, &patch, "agent-B", &upd));
    /* owner: succeeds */
    TEST_ASSERT_EQUAL_INT(AEGIS_OK,
                          qe_update(&g_db, sid, &patch, "agent-A", &upd));
    record_free(&upd);

    /* An episodic record from the wrong tenant is NOT_FOUND, never IMMUTABLE. */
    MemoryRecord ep = make_input(MEM_EPISODIC, "A's event");
    ep.agent_id = strdup("agent-A");
    MemoryRecord oe;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(&g_db, &ep, NULL, 0, &oe));
    uint64_t eid = oe.id;
    free(ep.agent_id);
    record_free(&oe);
    TEST_ASSERT_EQUAL_INT(AEGIS_ERR_NOT_FOUND,
                          qe_update(&g_db, eid, &patch, "agent-B", &upd));
    TEST_ASSERT_EQUAL_INT(AEGIS_ERR_IMMUTABLE,
                          qe_update(&g_db, eid, &patch, "agent-A", &upd));

    /* relate: both endpoints must be owned */
    TEST_ASSERT_EQUAL_INT(AEGIS_ERR_NOT_FOUND,
                          qe_relate(&g_db, sid, eid, "x", "agent-B"));
    TEST_ASSERT_EQUAL_INT(AEGIS_OK,
                          qe_relate(&g_db, sid, eid, "x", "agent-A"));

    /* delete: wrong tenant is NOT_FOUND, owner succeeds */
    TEST_ASSERT_EQUAL_INT(AEGIS_ERR_NOT_FOUND, qe_delete(&g_db, sid, "agent-B"));
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_delete(&g_db, sid, "agent-A"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_insert_get_episodic);
    RUN_TEST(test_get_not_found);
    RUN_TEST(test_episodic_update_rejected);
    RUN_TEST(test_semantic_update);
    RUN_TEST(test_search_by_time_and_tags);
    RUN_TEST(test_semantic_search);
    RUN_TEST(test_working_memory_promote);
    RUN_TEST(test_relate_and_traverse);
    RUN_TEST(test_agent_namespace_filter);
    RUN_TEST(test_ns_scoped_writes);
    return UNITY_END();
}