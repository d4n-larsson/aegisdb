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

/* Insert a semantic record with a 3-D embedding and one tag. Returns its id. */
static uint64_t insert_vec_tag(AegisDB *db, float *emb, const char *tag) {
    MemoryRecord in = make_input(MEM_SEMANTIC, "v");
    in.embedding = emb; /* borrowed */
    in.embedding_dim = 3;
    const char *tags[] = {tag};
    record_set_tags(&in, tags, 1);
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(db, &in, NULL, 0, &out));
    uint64_t id = out.id;
    in.data = NULL; /* borrowed literal */
    in.data_len = 0;
    in.embedding = NULL; /* borrowed */
    in.embedding_dim = 0;
    record_free(&in); /* frees the allocated tags */
    record_free(&out);
    return id;
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

/* qe_search must return exactly the top_k best candidates in rank order even
 * when far more match — exercising the bounded partial selection. Six semantic
 * vectors at increasing angle from the query have strictly decreasing
 * similarity; top_k=3 must yield the three nearest, in descending order. */
static void test_search_top_k_selection(void) {
    /* y-offsets chosen so cosine to (1,0,0) is strictly decreasing */
    float ys[6] = {0.0f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    uint64_t ids[6];
    int order[6] = {3, 0, 5, 2, 4, 1}; /* insert scrambled */
    for (int k = 0; k < 6; k++) {
        int i = order[k];
        float v[3] = {1.0f, ys[i], 0.0f};
        MemoryRecord in = make_input(MEM_SEMANTIC, "v");
        in.embedding = v;
        in.embedding_dim = 3;
        MemoryRecord out;
        TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(&g_db, &in, NULL, 0, &out));
        ids[i] = out.id;
        record_free(&out);
    }

    SearchParams p = {0};
    float query[3] = {1.0f, 0.0f, 0.0f};
    p.embedding = query;
    p.embedding_dim = 3;
    p.top_k = 3;
    MemoryRecord *recs = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(&g_db, &p, &recs, &n));
    TEST_ASSERT_EQUAL_size_t(3, n);
    /* nearest three are y=0,0.25,0.5 -> ids[0],ids[1],ids[2], in that order */
    TEST_ASSERT_EQUAL_UINT64(ids[0], recs[0].id);
    TEST_ASSERT_EQUAL_UINT64(ids[1], recs[1].id);
    TEST_ASSERT_EQUAL_UINT64(ids[2], recs[2].id);
    for (size_t i = 0; i < n; i++) record_free(&recs[i]);
    free(recs);
}

/* A selective post-filter must not silently lose matches that sit beyond the
 * initial vector over-fetch: the query is closest to a large 'common' cluster,
 * but we filter for a few 'rare' records that are farther away. The first
 * 4*top_k+32 candidates are all 'common', so without widening this returns 0;
 * the widening loop keeps fetching until the 'rare' matches are found. */
static void test_search_filtered_widening(void) {
    float q[3] = {1.0f, 0.0f, 0.0f};
    for (int i = 0; i < 60; i++) {
        float v[3] = {1.0f, 0.001f * (float)(i + 1), 0.0f}; /* very near q */
        insert_vec_tag(&g_db, v, "common");
    }
    uint64_t rare[3];
    for (int i = 0; i < 3; i++) {
        float v[3] = {1.0f, 0.5f + 0.02f * (float)i, 0.0f}; /* farther from q */
        rare[i] = insert_vec_tag(&g_db, v, "rare");
    }

    SearchParams p = {0};
    p.embedding = q;
    p.embedding_dim = 3;
    p.top_k = 3;
    const char *tags[] = {"rare"};
    p.tags = tags;
    p.tag_count = 1;
    p.match_all = 1;

    MemoryRecord *recs = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(&g_db, &p, &recs, &n));
    TEST_ASSERT_EQUAL_size_t(3, n); /* found despite being past the over-fetch */
    for (size_t i = 0; i < n; i++) {
        int is_rare = 0;
        for (int j = 0; j < 3; j++)
            if (recs[i].id == rare[j]) is_rare = 1;
        TEST_ASSERT_TRUE(is_rare);
        record_free(&recs[i]);
    }
    free(recs);

    /* a filter matching nothing terminates cleanly (no hang, empty result) */
    const char *ghost[] = {"ghost"};
    p.tags = ghost;
    recs = NULL;
    n = 99;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(&g_db, &p, &recs, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    free(recs);
}

/* offset pages the ranked result set; min_score gates on cosine similarity. */
static void test_search_offset_and_min_score(void) {
    /* five vectors at increasing angle from the query -> strictly decreasing
     * similarity, so their rank order (by similarity) is ids[0..4]. */
    float q[3] = {1.0f, 0.0f, 0.0f};
    float ys[5] = {0.0f, 0.2f, 0.5f, 1.0f, 3.0f};
    uint64_t ids[5];
    for (int i = 0; i < 5; i++) {
        float v[3] = {1.0f, ys[i], 0.0f};
        ids[i] = insert_vec_tag(&g_db, v, "s");
    }

    SearchParams p = {0};
    p.embedding = q;
    p.embedding_dim = 3;
    p.top_k = 2;

    /* page 1 (offset 0): the two nearest, in order */
    MemoryRecord *r = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(&g_db, &p, &r, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT64(ids[0], r[0].id);
    TEST_ASSERT_EQUAL_UINT64(ids[1], r[1].id);
    for (size_t i = 0; i < n; i++) record_free(&r[i]);
    free(r);

    /* page 2 (offset 2): the next two */
    p.offset = 2;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(&g_db, &p, &r, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT64(ids[2], r[0].id);
    TEST_ASSERT_EQUAL_UINT64(ids[3], r[1].id);
    for (size_t i = 0; i < n; i++) record_free(&r[i]);
    free(r);

    /* offset past the end -> empty, clean */
    p.offset = 99;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(&g_db, &p, &r, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    free(r);

    /* min_score: only the near-aligned vectors clear a high cosine floor. */
    p.offset = 0;
    p.top_k = 10;
    p.has_min_score = 1;
    p.min_score = 0.95f; /* ids[0] (sim 1.0) and ids[1] (~0.98) qualify; rest not */
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(&g_db, &p, &r, &n));
    TEST_ASSERT_TRUE(n >= 1 && n <= 2);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(r[i].id == ids[0] || r[i].id == ids[1]);
        record_free(&r[i]);
    }
    free(r);
}

/* Recency decay: two records identical in direction and importance, separated
 * in age by a real interval. Without decay their scores tie (ordering
 * unspecified); with a short half_life the fresher one must rank first, since
 * recency is then the only differentiator. */
static void test_search_recency_decay(void) {
    float v[3] = {1.0f, 0.0f, 0.0f}; /* both aligned with the query, same imp */
    uint64_t old_id = insert_vec_tag(&g_db, v, "d");
    usleep(800000); /* 0.8s so `updated` differs meaningfully vs a ~1s half-life */
    uint64_t new_id = insert_vec_tag(&g_db, v, "d");

    SearchParams p = {0};
    float q[3] = {1.0f, 0.0f, 0.0f};
    p.embedding = q;
    p.embedding_dim = 3;
    p.top_k = 2;

    /* both come back regardless (no decay yet) */
    MemoryRecord *r = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(&g_db, &p, &r, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    for (size_t i = 0; i < n; i++) record_free(&r[i]);
    free(r);

    /* with a 1s half-life, the ~0.8s-older record decays to ~0.57 of the fresh
     * one, so the fresh record ranks first */
    p.half_life_ms = 1000;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(&g_db, &p, &r, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT64(new_id, r[0].id);
    TEST_ASSERT_EQUAL_UINT64(old_id, r[1].id);
    for (size_t i = 0; i < n; i++) record_free(&r[i]);
    free(r);
}

/* Insert an episodic record with one tag and an optional namespace. */
static uint64_t insert_tag_ns(AegisDB *db, const char *tag, const char *ns) {
    MemoryRecord in = make_input(MEM_EPISODIC, "c");
    const char *tags[] = {tag};
    record_set_tags(&in, tags, 1);
    if (ns) in.agent_id = strdup(ns);
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(db, &in, NULL, 0, &out));
    uint64_t id = out.id;
    in.data = NULL;
    in.data_len = 0;
    record_free(&in);
    record_free(&out);
    return id;
}

/* qe_count returns the number of live records matching the filters, and is
 * namespace-scoped when agent_id is set. */
static void test_count(void) {
    insert_tag_ns(&g_db, "x", NULL);
    insert_tag_ns(&g_db, "x", NULL);
    insert_tag_ns(&g_db, "y", NULL);
    uint64_t owned = insert_tag_ns(&g_db, "x", "tenant-A");

    SearchParams p = {0};
    size_t n = 0;

    const char *x[] = {"x"};
    p.tags = x;
    p.tag_count = 1;
    p.match_all = 1;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_count(&g_db, &p, &n));
    TEST_ASSERT_EQUAL_size_t(3, n); /* two global + tenant-A's */

    /* namespace-scoped: only tenant-A's "x" record */
    p.agent_id = "tenant-A";
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_count(&g_db, &p, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);

    /* deleting the owned record drops the scoped count to 0 */
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_delete(&g_db, owned, "tenant-A"));
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_count(&g_db, &p, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);

    /* a tag matching nothing counts 0 */
    const char *z[] = {"z"};
    p.agent_id = NULL;
    p.tags = z;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_count(&g_db, &p, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
}

/* A TTL'd episodic/semantic record is recallable before its horizon and reads
 * back as not-found after it (lazy filter), for get and search alike. */
static void test_ttl_lazy_expiry(void) {
    MemoryRecord in = make_input(MEM_EPISODIC, "ephemeral");
    const char *tags[] = {"tmp"};
    record_set_tags(&in, tags, 1);
    MemoryRecord out;
    /* 50ms TTL */
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(&g_db, &in, NULL, 50, &out));
    uint64_t id = out.id;
    in.data = NULL;
    in.data_len = 0;
    record_free(&in);
    record_free(&out);

    /* before expiry: gettable and searchable */
    MemoryRecord got;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_get(&g_db, id, NULL, &got));
    record_free(&got);

    usleep(80000); /* 80ms > 50ms TTL */

    TEST_ASSERT_EQUAL_INT(AEGIS_ERR_NOT_FOUND, qe_get(&g_db, id, NULL, &got));

    SearchParams p = {0};
    const char *q[] = {"tmp"};
    p.tags = q;
    p.tag_count = 1;
    p.match_all = 1;
    p.top_k = 10;
    MemoryRecord *recs = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_search(&g_db, &p, &recs, &n));
    TEST_ASSERT_EQUAL_size_t(0, n); /* expired record not returned */
    free(recs);

    /* a record with no TTL is unaffected */
    MemoryRecord in2 = make_input(MEM_EPISODIC, "permanent");
    MemoryRecord out2;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(&g_db, &in2, NULL, 0, &out2));
    uint64_t id2 = out2.id;
    in2.data = NULL;
    in2.data_len = 0;
    record_free(&in2);
    record_free(&out2);
    usleep(80000);
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_get(&g_db, id2, NULL, &got)); /* still there */
    record_free(&got);
}

/* The expiry sweep tombstones expired records: after it runs, count drops and
 * the record is gone (not merely hidden). */
static void test_ttl_sweep_tombstones(void) {
    MemoryRecord in = make_input(MEM_EPISODIC, "sweepable");
    const char *tags[] = {"sw"};
    record_set_tags(&in, tags, 1);
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(AEGIS_OK, qe_insert(&g_db, &in, NULL, 10, &out));
    uint64_t id = out.id;
    in.data = NULL;
    in.data_len = 0;
    record_free(&in);
    record_free(&out);

    usleep(30000); /* past the 10ms TTL */
    size_t swept = qe_sweep_expired(&g_db, db_now_ms());
    TEST_ASSERT_TRUE(swept >= 1);

    /* a second sweep finds nothing new (already tombstoned) */
    TEST_ASSERT_EQUAL_size_t(0, qe_sweep_expired(&g_db, db_now_ms()));

    MemoryRecord got;
    TEST_ASSERT_EQUAL_INT(AEGIS_ERR_NOT_FOUND, qe_get(&g_db, id, NULL, &got));
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
    RUN_TEST(test_search_top_k_selection);
    RUN_TEST(test_search_filtered_widening);
    RUN_TEST(test_search_offset_and_min_score);
    RUN_TEST(test_search_recency_decay);
    RUN_TEST(test_count);
    RUN_TEST(test_ttl_lazy_expiry);
    RUN_TEST(test_ttl_sweep_tombstones);
    RUN_TEST(test_working_memory_promote);
    RUN_TEST(test_relate_and_traverse);
    RUN_TEST(test_agent_namespace_filter);
    RUN_TEST(test_ns_scoped_writes);
    return UNITY_END();
}