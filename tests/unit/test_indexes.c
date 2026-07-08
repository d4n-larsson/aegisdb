/* Unit tests for the secondary indexes: time, tag, and semantic. */
#include <stdlib.h>

#include "aegisdb/semantic_index.h"
#include "aegisdb/tag_index.h"
#include "aegisdb/time_index.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- TimeIndex --------------------------------------------------------- */

static void test_time_range_chronological(void) {
    TimeIndex *t = time_index_create();
    /* Insert out of order; range results must come back chronological. */
    time_index_add(t, 300, 3);
    time_index_add(t, 100, 1);
    time_index_add(t, 200, 2);
    time_index_add(t, 400, 4);

    uint64_t *ids = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, time_index_range(t, 150, 350, 100, &ids, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT64(2, ids[0]); /* created=200 */
    TEST_ASSERT_EQUAL_UINT64(3, ids[1]); /* created=300 */
    free(ids);

    /* Full range returns everything in order. */
    TEST_ASSERT_EQUAL_INT(0, time_index_range(t, 0, 1000, 100, &ids, &n));
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_UINT64(1, ids[0]);
    TEST_ASSERT_EQUAL_UINT64(4, ids[3]);
    free(ids);
    time_index_free(t);
}

static void test_time_range_respects_max(void) {
    TimeIndex *t = time_index_create();
    for (uint64_t i = 1; i <= 10; i++) time_index_add(t, i * 10, i);
    uint64_t *ids = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, time_index_range(t, 0, 1000, 3, &ids, &n));
    TEST_ASSERT_EQUAL_size_t(3, n);
    free(ids);
    time_index_free(t);
}

/* ---- TagIndex ---------------------------------------------------------- */

static void test_tag_intersection_and_union(void) {
    TagIndex *t = tag_index_create();
    /* id 1: {user,pref}  id 2: {user}  id 3: {pref} */
    tag_index_add(t, "user", 1);
    tag_index_add(t, "pref", 1);
    tag_index_add(t, "user", 2);
    tag_index_add(t, "pref", 3);

    const char *q[] = {"user", "pref"};
    uint64_t *ids = NULL;
    size_t n = 0;

    /* match_all (intersection) -> only id 1 has both. */
    TEST_ASSERT_EQUAL_INT(0, tag_index_query(t, q, 2, 1, &ids, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(1, ids[0]);
    free(ids);

    /* union -> 1,2,3 sorted ascending. */
    TEST_ASSERT_EQUAL_INT(0, tag_index_query(t, q, 2, 0, &ids, &n));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_UINT64(1, ids[0]);
    TEST_ASSERT_EQUAL_UINT64(2, ids[1]);
    TEST_ASSERT_EQUAL_UINT64(3, ids[2]);
    free(ids);

    tag_index_free(t);
}

static void test_tag_remove(void) {
    TagIndex *t = tag_index_create();
    tag_index_add(t, "x", 1);
    tag_index_add(t, "x", 2);
    tag_index_remove(t, "x", 1);
    const char *q[] = {"x"};
    uint64_t *ids = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, tag_index_query(t, q, 1, 1, &ids, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(2, ids[0]);
    free(ids);
    tag_index_free(t);
}

/* ---- SemanticIndex ----------------------------------------------------- */

static void test_semantic_topk_ordering(void) {
    const size_t dim = 3;
    SemanticIndex *s = semantic_index_create(dim, 0, 0, 0, 0);
    float a[] = {1.0f, 0.0f, 0.0f};  /* id 1 */
    float b[] = {0.9f, 0.1f, 0.0f};  /* id 2 (close to query) */
    float c[] = {0.0f, 1.0f, 0.0f};  /* id 3 (orthogonal-ish) */
    semantic_index_add(s, 1, a, 1, dim);
    semantic_index_add(s, 2, b, 1, dim);
    semantic_index_add(s, 3, c, 1, dim);

    float query[] = {1.0f, 0.0f, 0.0f};
    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(
        0, semantic_index_search(s, query, dim, 2, &ids, &scores, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT64(1, ids[0]); /* exact match ranks first */
    TEST_ASSERT_EQUAL_UINT64(2, ids[1]);
    /* Scores must be in non-increasing order. */
    TEST_ASSERT_TRUE(scores[0] >= scores[1]);
    free(ids);
    free(scores);
    semantic_index_free(s);
}

/* Removing an entry must shrink the index and exclude it from results, and
 * remove/re-add churn must not grow the index (no leaked/dead slots). */
static void test_semantic_remove_reclaims(void) {
    const size_t dim = 2;
    SemanticIndex *s = semantic_index_create(dim, 0, 0, 0, 0);
    float v1[] = {1.0f, 0.0f};
    float v2[] = {0.0f, 1.0f};
    semantic_index_add(s, 1, v1, 1, dim);
    semantic_index_add(s, 2, v2, 1, dim);
    TEST_ASSERT_EQUAL_size_t(2, semantic_index_count(s));

    semantic_index_remove(s, 1);
    TEST_ASSERT_EQUAL_size_t(1, semantic_index_count(s)); /* slot reclaimed */

    /* id 1 must no longer appear in search results. */
    float q[] = {1.0f, 0.0f};
    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0,
        semantic_index_search(s, q, dim, 10, &ids, &scores, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(2, ids[0]);
    free(ids);
    free(scores);

    /* remove/re-add churn stays bounded (no dead-slot accumulation). */
    for (int i = 0; i < 100; i++) {
        semantic_index_add(s, 1, v1, 1, dim);
        semantic_index_remove(s, 1);
    }
    TEST_ASSERT_EQUAL_size_t(1, semantic_index_count(s));
    semantic_index_free(s);
}

/* Re-adding an existing id replaces its vector in place (no duplicate slot),
 * and search reflects the new direction. */
static void test_semantic_overwrite_in_place(void) {
    const size_t dim = 2;
    SemanticIndex *s = semantic_index_create(dim, 0, 0, 0, 0);
    float along_x[] = {1.0f, 0.0f};
    float along_y[] = {0.0f, 1.0f};
    semantic_index_add(s, 7, along_x, 1, dim);
    TEST_ASSERT_EQUAL_size_t(1, semantic_index_count(s));

    /* overwrite id 7 to point along y */
    semantic_index_add(s, 7, along_y, 1, dim);
    TEST_ASSERT_EQUAL_size_t(1, semantic_index_count(s)); /* no new slot */

    float qy[] = {0.0f, 1.0f};
    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(
        0, semantic_index_search(s, qy, dim, 1, &ids, &scores, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(7, ids[0]);
    TEST_ASSERT_TRUE(scores[0] > 0.99f); /* now aligned with y */
    free(ids);
    free(scores);
    semantic_index_free(s);
}

/* Build a large index, then remove a scattered subset. The swap-remove must
 * keep the id->slot map consistent: every surviving id stays findable and every
 * removed id disappears. A stale dense-index mapping or a broken probe chain
 * (from the open-addressing delete) would surface here. */
static void test_semantic_bulk_add_remove_consistency(void) {
    const size_t dim = 1;
    const uint64_t N = 1000;
    SemanticIndex *s = semantic_index_create(dim, 0, 0, 0, 0);
    for (uint64_t id = 1; id <= N; id++) {
        float v[] = {(float)id};
        TEST_ASSERT_EQUAL_INT(0, semantic_index_add(s, id, v, 1, dim));
    }
    TEST_ASSERT_EQUAL_size_t((size_t)N, semantic_index_count(s));

    /* remove every third id */
    size_t removed = 0;
    for (uint64_t id = 1; id <= N; id++)
        if (id % 3 == 0) {
            semantic_index_remove(s, id);
            removed++;
        }
    TEST_ASSERT_EQUAL_size_t((size_t)N - removed, semantic_index_count(s));

    /* Pull the whole index back and confirm exactly the survivors remain. */
    float q[] = {1.0f};
    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(
        0, semantic_index_search(s, q, dim, (size_t)N, &ids, &scores, &n));
    TEST_ASSERT_EQUAL_size_t((size_t)N - removed, n);

    char seen[1001] = {0};
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(ids[i] >= 1 && ids[i] <= N);
        TEST_ASSERT_TRUE(ids[i] % 3 != 0); /* no removed id resurfaces */
        TEST_ASSERT_FALSE(seen[ids[i]]);   /* no duplicates */
        seen[ids[i]] = 1;
    }
    for (uint64_t id = 1; id <= N; id++)
        if (id % 3 != 0) TEST_ASSERT_TRUE(seen[id]); /* every survivor present */
    free(ids);
    free(scores);
    semantic_index_free(s);
}

/* With many more vectors than top_k, the partial selection must return exactly
 * the k highest-similarity ids in descending order. id i has vector (1, i-1),
 * so cosine to the query (1,0) strictly decreases as i grows: the top k are
 * ids 1..k. Inserted in a scrambled order to defeat any reliance on it. */
static void test_semantic_topk_partial_selection(void) {
    const size_t dim = 2;
    const uint64_t N = 200, K = 5;
    SemanticIndex *s = semantic_index_create(dim, 0, 0, 0, 0);
    /* scramble insertion order with a coprime stride */
    for (uint64_t step = 0; step < N; step++) {
        uint64_t id = 1 + (step * 73 + 11) % N;
        float v[] = {1.0f, (float)(id - 1)};
        semantic_index_add(s, id, v, 1, dim);
    }
    TEST_ASSERT_EQUAL_size_t((size_t)N, semantic_index_count(s));

    float q[] = {1.0f, 0.0f};
    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(
        0, semantic_index_search(s, q, dim, K, &ids, &scores, &n));
    TEST_ASSERT_EQUAL_size_t((size_t)K, n);
    for (uint64_t i = 0; i < K; i++) {
        TEST_ASSERT_EQUAL_UINT64(i + 1, ids[i]);          /* exact top-k set */
        if (i > 0) TEST_ASSERT_TRUE(scores[i - 1] >= scores[i]); /* descending */
    }
    free(ids);
    free(scores);
    semantic_index_free(s);
}

/* Crossing ann_threshold switches search to the HNSW graph. With well-separated
 * vectors (id i -> (1, i-1), monotonic angle from the query) the approximate
 * path still returns the exact top-k, and remove/overwrite stay correct through
 * it. */
static void test_semantic_hnsw_path(void) {
    const size_t dim = 2;
    const uint64_t N = 300, K = 5;
    SemanticIndex *s = semantic_index_create(dim, 16, 100, 0, 0); /* HNSW once n>=16 */
    for (uint64_t id = 1; id <= N; id++) {
        float v[] = {1.0f, (float)(id - 1)};
        TEST_ASSERT_EQUAL_INT(0, semantic_index_add(s, id, v, 1, dim));
    }
    TEST_ASSERT_EQUAL_size_t((size_t)N, semantic_index_count(s));
    /* The build is deferred on a live server; drive it directly so the searches
     * and the remove/overwrite below exercise the HNSW graph, not the dense scan. */
    TEST_ASSERT_TRUE(semantic_index_needs_build(s));
    TEST_ASSERT_EQUAL_INT(0, semantic_index_build_now(s));

    float q[] = {1.0f, 0.0f};
    uint64_t *ids = NULL;
    float *sc = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, semantic_index_search(s, q, dim, K, &ids, &sc, &n));
    TEST_ASSERT_EQUAL_size_t((size_t)K, n);
    for (uint64_t i = 0; i < K; i++) {
        TEST_ASSERT_EQUAL_UINT64(i + 1, ids[i]);            /* nearest are 1..5 */
        if (i > 0) TEST_ASSERT_TRUE(sc[i - 1] >= sc[i]);
    }
    free(ids);
    free(sc);

    /* remove the nearest (id 1) through the HNSW path: id 2 becomes nearest */
    semantic_index_remove(s, 1);
    TEST_ASSERT_EQUAL_size_t((size_t)N - 1, semantic_index_count(s));
    TEST_ASSERT_EQUAL_INT(0, semantic_index_search(s, q, dim, K, &ids, &sc, &n));
    TEST_ASSERT_EQUAL_UINT64(2, ids[0]);
    for (size_t i = 0; i < n; i++) TEST_ASSERT_TRUE(ids[i] != 1);
    free(ids);
    free(sc);

    /* overwrite id 2 to point away from q: it drops out, id 3 becomes nearest */
    float away[] = {0.0f, 1.0f};
    TEST_ASSERT_EQUAL_INT(0, semantic_index_add(s, 2, away, 1, dim));
    TEST_ASSERT_EQUAL_INT(0, semantic_index_search(s, q, dim, K, &ids, &sc, &n));
    TEST_ASSERT_EQUAL_UINT64(3, ids[0]);
    free(ids);
    free(sc);
    semantic_index_free(s);
}

/* Best-of-N (#85): a record with several vectors is matched by whichever is
 * closest and returned once. `threshold` selects the exact (0=default high) or
 * HNSW (small) path so both are exercised. */
static void multivector_best_of_n(size_t threshold) {
    const size_t dim = 3;
    SemanticIndex *s = semantic_index_create(dim, threshold, 100, 0, 0);
    float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    /* record 1 owns two vectors (x and y); record 2 owns z */
    float r1[6] = {1, 0, 0, 0, 1, 0};
    TEST_ASSERT_EQUAL_INT(0, semantic_index_add(s, 1, r1, 2, dim));
    TEST_ASSERT_EQUAL_INT(0, semantic_index_add(s, 2, axes[2], 1, dim));
    /* HNSW variant (small threshold): build the deferred graph so the search
     * runs against it; exact variant (high threshold) leaves the dense scan. */
    if (semantic_index_needs_build(s))
        TEST_ASSERT_EQUAL_INT(0, semantic_index_build_now(s));

    uint64_t *ids = NULL;
    float *sc = NULL;
    size_t n = 0;
    /* query along y — record 1's *second* vector — must return record 1 once */
    TEST_ASSERT_EQUAL_INT(
        0, semantic_index_search(s, axes[1], dim, 5, &ids, &sc, &n));
    int seen1 = 0, dup1 = 0;
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == 1) { if (seen1) dup1 = 1; seen1 = 1; }
    }
    TEST_ASSERT_TRUE(seen1);      /* found by its non-primary vector */
    TEST_ASSERT_FALSE(dup1);      /* returned once, not per-vector */
    TEST_ASSERT_EQUAL_UINT64(1, ids[0]); /* best match is record 1 (sim ~1) */
    TEST_ASSERT_TRUE(sc[0] > 0.99f);
    free(ids);
    free(sc);

    /* removing record 1 drops *both* its vectors */
    semantic_index_remove(s, 1);
    TEST_ASSERT_EQUAL_INT(
        0, semantic_index_search(s, axes[1], dim, 5, &ids, &sc, &n));
    for (size_t i = 0; i < n; i++) TEST_ASSERT_TRUE(ids[i] != 1);
    free(ids);
    free(sc);
    semantic_index_free(s);
}
static void test_semantic_multivector_dense(void) { multivector_best_of_n(0); }
static void test_semantic_multivector_hnsw(void) { multivector_best_of_n(2); }

/* The deferred off-lock build must fold in add/remove ops that race the build:
 * begin() snapshots and starts journalling, mutations after it are recorded as
 * deltas, and commit() replays them so the installed graph matches the live
 * dense state. Drives the phases directly (the maintenance thread does this on
 * a live server, taking the index lock around begin/take/commit). */
static void test_semantic_deferred_build_catch_up(void) {
    const size_t dim = 2;
    SemanticIndex *s = semantic_index_create(dim, 16, 100, 0, 0); /* HNSW once n>=16 */
    for (uint64_t id = 1; id <= 20; id++) {
        float v[] = {1.0f, (float)(id - 1)};
        TEST_ASSERT_EQUAL_INT(0, semantic_index_add(s, id, v, 1, dim));
    }
    TEST_ASSERT_TRUE(semantic_index_needs_build(s));

    /* Phase 2: snapshot 20 vectors, enter building mode. */
    SemBuildJob *job = semantic_index_build_begin(s);
    TEST_ASSERT_NOT_NULL(job);
    TEST_ASSERT_FALSE(semantic_index_needs_build(s)); /* build now in flight */

    /* Writes that race the build: remove the query's nearest (id 1) and add a
     * brand-new nearest (id 100 exactly on the query axis). Both are journalled. */
    semantic_index_remove(s, 1);
    float onaxis[] = {1.0f, 0.0f};
    TEST_ASSERT_EQUAL_INT(0, semantic_index_add(s, 100, onaxis, 1, dim));
    /* Dense (still authoritative) already reflects them. */
    TEST_ASSERT_EQUAL_size_t(20, semantic_index_count(s)); /* -1 (id1) +1 (id100) */

    /* Phase 3: build from the snapshot (which predates the two mutations). */
    TEST_ASSERT_EQUAL_INT(0, semantic_index_build_run(job));
    /* Phase 4: fold the journalled deltas into the graph. */
    TEST_ASSERT_EQUAL_size_t(2, semantic_index_build_take_deltas(s, job));
    TEST_ASSERT_EQUAL_INT(0, semantic_index_build_apply(job));
    /* Phase 5: install. */
    TEST_ASSERT_EQUAL_INT(0, semantic_index_build_commit(s, job));

    /* The graph is now live and reflects the racing writes. */
    TEST_ASSERT_EQUAL_size_t(20, semantic_index_count(s));
    float q[] = {1.0f, 0.0f};
    uint64_t *ids = NULL;
    float *sc = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, semantic_index_search(s, q, dim, 5, &ids, &sc, &n));
    TEST_ASSERT_EQUAL_UINT64(100, ids[0]); /* the raced-in add is the nearest */
    for (size_t i = 0; i < n; i++) TEST_ASSERT_TRUE(ids[i] != 1); /* raced-out gone */
    free(ids);
    free(sc);
    semantic_index_free(s);
}

/* Multi-shard build: with a small per-shard target the deferred build splits
 * into several independent HNSW shards (one thread each). Search must fan out
 * over all shards and merge, and add/remove must route to the right shard. On
 * well-separated data (id i -> (1, i-1)) the merged approximate result is still
 * the exact top-k, and remove/overwrite stay correct across the shard split. */
static void test_semantic_sharded_build(void) {
    const size_t dim = 2;
    const uint64_t N = 200, K = 5;
    /* small shard target (like --ann-shard-target) forces many shards at small N */
    SemanticIndex *s = semantic_index_create(dim, 16, 100, 0, /*shard_target=*/16);
    for (uint64_t id = 1; id <= N; id++) {
        float v[] = {1.0f, (float)(id - 1)};
        TEST_ASSERT_EQUAL_INT(0, semantic_index_add(s, id, v, 1, dim));
    }
    TEST_ASSERT_TRUE(semantic_index_needs_build(s));

    /* Deferred build, no racing writes -> straight begin/run/commit. */
    SemBuildJob *job = semantic_index_build_begin(s);
    TEST_ASSERT_NOT_NULL(job);
    TEST_ASSERT_EQUAL_INT(0, semantic_index_build_run(job));
    TEST_ASSERT_EQUAL_INT(0, semantic_index_build_commit(s, job));
    TEST_ASSERT_EQUAL_size_t((size_t)N, semantic_index_count(s));

    float q[] = {1.0f, 0.0f};
    uint64_t *ids = NULL;
    float *sc = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, semantic_index_search(s, q, dim, K, &ids, &sc, &n));
    TEST_ASSERT_EQUAL_size_t((size_t)K, n);
    for (uint64_t i = 0; i < K; i++) {
        TEST_ASSERT_EQUAL_UINT64(i + 1, ids[i]);       /* exact top-k across shards */
        if (i > 0) TEST_ASSERT_TRUE(sc[i - 1] >= sc[i]);
    }
    free(ids);
    free(sc);

    /* remove the nearest (routes to its shard); id 2 becomes nearest */
    semantic_index_remove(s, 1);
    TEST_ASSERT_EQUAL_size_t((size_t)N - 1, semantic_index_count(s));
    TEST_ASSERT_EQUAL_INT(0, semantic_index_search(s, q, dim, K, &ids, &sc, &n));
    TEST_ASSERT_EQUAL_UINT64(2, ids[0]);
    for (size_t i = 0; i < n; i++) TEST_ASSERT_TRUE(ids[i] != 1);
    free(ids);
    free(sc);

    /* overwrite id 2 to point away from q: it drops out, id 3 becomes nearest */
    float away[] = {0.0f, 1.0f};
    TEST_ASSERT_EQUAL_INT(0, semantic_index_add(s, 2, away, 1, dim));
    TEST_ASSERT_EQUAL_INT(0, semantic_index_search(s, q, dim, K, &ids, &sc, &n));
    TEST_ASSERT_EQUAL_UINT64(3, ids[0]);
    free(ids);
    free(sc);
    semantic_index_free(s);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_time_range_chronological);
    RUN_TEST(test_time_range_respects_max);
    RUN_TEST(test_tag_intersection_and_union);
    RUN_TEST(test_tag_remove);
    RUN_TEST(test_semantic_topk_ordering);
    RUN_TEST(test_semantic_remove_reclaims);
    RUN_TEST(test_semantic_overwrite_in_place);
    RUN_TEST(test_semantic_bulk_add_remove_consistency);
    RUN_TEST(test_semantic_topk_partial_selection);
    RUN_TEST(test_semantic_hnsw_path);
    RUN_TEST(test_semantic_multivector_dense);
    RUN_TEST(test_semantic_multivector_hnsw);
    RUN_TEST(test_semantic_deferred_build_catch_up);
    RUN_TEST(test_semantic_sharded_build);
    return UNITY_END();
}