/* Unit tests for the HNSW ANN index (#38). Because HNSW is approximate, the
 * recall test compares its top-k against a brute-force exact ground truth and
 * asserts a recall floor rather than exact equality. */
#include <math.h>
#include <stdlib.h>

#include "aegisdb/hnsw.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define DIM 16

static uint64_t g_rng;
static float frand(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (float)((g_rng >> 11) * (1.0 / 9007199254740992.0)) * 2.0f - 1.0f;
}
static void rand_vec(float *v) {
    for (int i = 0; i < DIM; i++) v[i] = frand();
}

static float cosine(const float *a, const float *b) {
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < DIM; i++) {
        dot += (double)a[i] * b[i];
        na += (double)a[i] * a[i];
        nb += (double)b[i] * b[i];
    }
    double den = sqrt(na) * sqrt(nb);
    return den > 0 ? (float)(dot / den) : 0.0f;
}

/* Exact top-k ids by cosine similarity over vecs[0..n). */
static void brute_top_k(const float *vecs, size_t n, const float *q, size_t k,
                        uint64_t *out_ids) {
    float *sims = malloc(n * sizeof(float));
    for (size_t i = 0; i < n; i++) sims[i] = cosine(q, &vecs[i * DIM]);
    for (size_t r = 0; r < k; r++) {
        size_t best = 0;
        for (size_t i = 1; i < n; i++)
            if (sims[i] > sims[best]) best = i;
        out_ids[r] = (uint64_t)best;
        sims[best] = -2.0f; /* exclude */
    }
    free(sims);
}

/* Average recall@k of HNSW against brute force over many random queries. */
static void test_hnsw_recall(void) {
    const size_t N = 2000, K = 10, Q = 50;
    g_rng = 0xC0FFEE123456789ULL;

    float *vecs = malloc(N * DIM * sizeof(float));
    for (size_t i = 0; i < N; i++) rand_vec(&vecs[i * DIM]);

    HnswParams p = {.M = 16, .ef_construction = 200, .ef_search = 100, .seed = 42};
    Hnsw *h = hnsw_create(DIM, &p);
    TEST_ASSERT_NOT_NULL(h);
    for (size_t i = 0; i < N; i++)
        TEST_ASSERT_EQUAL_INT(0, hnsw_add(h, i, &vecs[i * DIM], DIM));
    TEST_ASSERT_EQUAL_size_t(N, hnsw_count(h));

    size_t hits = 0;
    uint64_t truth[16];
    for (size_t qi = 0; qi < Q; qi++) {
        float q[DIM];
        rand_vec(q);
        brute_top_k(vecs, N, q, K, truth);

        uint64_t *ids = NULL;
        float *sc = NULL;
        size_t n = 0;
        TEST_ASSERT_EQUAL_INT(0, hnsw_search(h, q, DIM, K, 0, &ids, &sc, &n));
        TEST_ASSERT_EQUAL_size_t(K, n);
        for (size_t i = 1; i < n; i++)
            TEST_ASSERT_TRUE(sc[i - 1] >= sc[i]); /* scores descending */
        for (size_t i = 0; i < n; i++)
            for (size_t j = 0; j < K; j++)
                if (ids[i] == truth[j]) { hits++; break; }
        free(ids);
        free(sc);
    }
    double recall = (double)hits / (double)(Q * K);
    /* simple-neighbour HNSW at ef_search=100 comfortably clears this */
    TEST_ASSERT_TRUE_MESSAGE(recall >= 0.90, "recall@10 below 0.90");

    hnsw_free(h);
    free(vecs);
}

/* Removed ids never resurface; count tracks live nodes. */
static void test_hnsw_delete(void) {
    const size_t N = 500;
    g_rng = 0x1234ULL;
    float *vecs = malloc(N * DIM * sizeof(float));
    Hnsw *h = hnsw_create(DIM, NULL);
    for (size_t i = 0; i < N; i++) {
        rand_vec(&vecs[i * DIM]);
        hnsw_add(h, i, &vecs[i * DIM], DIM);
    }
    /* remove every even id */
    size_t removed = 0;
    for (size_t i = 0; i < N; i += 2) { hnsw_remove(h, i); removed++; }
    TEST_ASSERT_EQUAL_size_t(N - removed, hnsw_count(h));

    for (size_t qi = 0; qi < 20; qi++) {
        float q[DIM];
        rand_vec(q);
        uint64_t *ids = NULL;
        float *sc = NULL;
        size_t n = 0;
        TEST_ASSERT_EQUAL_INT(0, hnsw_search(h, q, DIM, 10, 0, &ids, &sc, &n));
        for (size_t i = 0; i < n; i++)
            TEST_ASSERT_TRUE(ids[i] % 2 == 1); /* no removed (even) id returned */
        free(ids);
        free(sc);
    }
    hnsw_free(h);
    free(vecs);
}

/* Re-adding an id replaces its vector: the new direction is found, the count
 * does not grow, and a query toward the old direction no longer returns it. */
static void test_hnsw_update(void) {
    Hnsw *h = hnsw_create(DIM, NULL);
    /* fill with noise so the graph is non-trivial */
    g_rng = 0x99ULL;
    float v[DIM];
    for (size_t i = 1; i < 200; i++) { rand_vec(v); hnsw_add(h, i, v, DIM); }

    float axis_a[DIM] = {0}, axis_b[DIM] = {0};
    axis_a[0] = 1.0f;
    axis_b[1] = 1.0f;
    hnsw_add(h, 1000, axis_a, DIM); /* id 1000 points along axis A */
    size_t before = hnsw_count(h);

    uint64_t *ids = NULL; float *sc = NULL; size_t n = 0;
    hnsw_search(h, axis_a, DIM, 1, 200, &ids, &sc, &n);
    TEST_ASSERT_EQUAL_UINT64(1000, ids[0]); /* nearest to A */
    free(ids); free(sc);

    hnsw_add(h, 1000, axis_b, DIM); /* re-point id 1000 to axis B */
    TEST_ASSERT_EQUAL_size_t(before, hnsw_count(h)); /* replaced, not added */

    hnsw_search(h, axis_b, DIM, 1, 200, &ids, &sc, &n);
    TEST_ASSERT_EQUAL_UINT64(1000, ids[0]); /* now nearest to B */
    free(ids); free(sc);
    hnsw_free(h);
}

/* Same seed + insertion order -> identical results. */
static void test_hnsw_reproducible(void) {
    const size_t N = 300;
    g_rng = 0x5555ULL;
    float *vecs = malloc(N * DIM * sizeof(float));
    for (size_t i = 0; i < N; i++) rand_vec(&vecs[i * DIM]);

    HnswParams p = {.seed = 7};
    uint64_t ids1[10], ids2[10];
    float q[DIM];
    g_rng = 0x777ULL;
    rand_vec(q);

    for (int pass = 0; pass < 2; pass++) {
        Hnsw *h = hnsw_create(DIM, &p);
        for (size_t i = 0; i < N; i++) hnsw_add(h, i, &vecs[i * DIM], DIM);
        uint64_t *ids = NULL; float *sc = NULL; size_t n = 0;
        hnsw_search(h, q, DIM, 10, 0, &ids, &sc, &n);
        TEST_ASSERT_EQUAL_size_t(10, n);
        for (size_t i = 0; i < n; i++)
            (pass == 0 ? ids1 : ids2)[i] = ids[i];
        free(ids); free(sc);
        hnsw_free(h);
    }
    for (int i = 0; i < 10; i++) TEST_ASSERT_EQUAL_UINT64(ids1[i], ids2[i]);
    free(vecs);
}

static void test_hnsw_empty(void) {
    Hnsw *h = hnsw_create(DIM, NULL);
    float q[DIM] = {1.0f};
    uint64_t *ids = NULL; float *sc = NULL; size_t n = 99;
    TEST_ASSERT_EQUAL_INT(0, hnsw_search(h, q, DIM, 5, 0, &ids, &sc, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    free(ids); free(sc);
    hnsw_free(h);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hnsw_empty);
    RUN_TEST(test_hnsw_recall);
    RUN_TEST(test_hnsw_delete);
    RUN_TEST(test_hnsw_update);
    RUN_TEST(test_hnsw_reproducible);
    return UNITY_END();
}