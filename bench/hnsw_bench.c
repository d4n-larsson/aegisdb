/* HNSW recall + latency benchmark (#55).
 *
 * Measures recall@k of the approximate index against a brute-force exact
 * ground truth at production embedding dimensions, plus build and query time.
 * Not part of `make test` (too heavy for CI) — run via `make bench` or directly:
 *
 *   ./build/bench/hnsw_bench [dim N queries M ef_construction ef_search k quantize]
 *
 * Defaults: dim=384 N=10000 queries=100 M=16 ef_construction=200 ef_search=50
 * k=10 quantize=0. Set quantize=1 to measure int8 recall vs the float32 default.
 *
 * Vectors are CLUSTERED (a mixture of Gaussians around random centers), not
 * uniform random: real text embeddings live on a lower-dimensional clustered
 * manifold where nearest neighbours are well-defined, whereas uniform-random
 * high-dim points are nearly equidistant (concentration of measure) and make
 * ANN recall look artificially terrible — unrepresentative of real workloads.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "aegisdb/hnsw.h"

static uint64_t g_rng = 0x123456789ABCDEFULL;
static float frand(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (float)((g_rng >> 11) * (1.0 / 9007199254740992.0)) * 2.0f - 1.0f;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static float cosine(const float *a, const float *b, size_t dim) {
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < dim; i++) {
        dot += (double)a[i] * b[i];
        na += (double)a[i] * a[i];
        nb += (double)b[i] * b[i];
    }
    double den = sqrt(na) * sqrt(nb);
    return den > 0 ? (float)(dot / den) : 0.0f;
}

/* exact top-k ids by cosine (partial selection) */
static void brute_top_k(const float *vecs, size_t n, size_t dim, const float *q,
                        size_t k, uint64_t *out) {
    float *sims = malloc(n * sizeof(float));
    for (size_t i = 0; i < n; i++) sims[i] = cosine(q, &vecs[i * dim], dim);
    for (size_t r = 0; r < k; r++) {
        size_t best = 0;
        for (size_t i = 1; i < n; i++)
            if (sims[i] > sims[best]) best = i;
        out[r] = (uint64_t)best;
        sims[best] = -2.0f;
    }
    free(sims);
}

int main(int argc, char **argv) {
    size_t dim = argc > 1 ? strtoul(argv[1], NULL, 10) : 384;
    size_t N = argc > 2 ? strtoul(argv[2], NULL, 10) : 10000;
    size_t Q = argc > 3 ? strtoul(argv[3], NULL, 10) : 100;
    size_t M = argc > 4 ? strtoul(argv[4], NULL, 10) : 16;
    size_t efc = argc > 5 ? strtoul(argv[5], NULL, 10) : 200;
    size_t efs = argc > 6 ? strtoul(argv[6], NULL, 10) : 50;
    size_t K = argc > 7 ? strtoul(argv[7], NULL, 10) : 10;
    int quantize = argc > 8 ? (int)strtoul(argv[8], NULL, 10) : 0;

    printf("dim=%zu N=%zu Q=%zu M=%zu ef_construction=%zu ef_search=%zu k=%zu "
           "quantize=%d\n",
           dim, N, Q, M, efc, efs, K, quantize);

    /* Clustered data: ~100 vectors per cluster, each a center + small noise. */
    size_t C = N / 100 ? N / 100 : 1;
    float *centers = malloc(C * dim * sizeof(float));
    for (size_t i = 0; i < C * dim; i++) centers[i] = frand();
    float *vecs = malloc(N * dim * sizeof(float));
    for (size_t i = 0; i < N; i++) {
        const float *c = &centers[(i % C) * dim];
        for (size_t d = 0; d < dim; d++) vecs[i * dim + d] = c[d] + 0.15f * frand();
    }

    HnswParams p = {.M = M, .ef_construction = efc, .ef_search = efs, .seed = 1,
                    .quantize = quantize};
    Hnsw *h = hnsw_create(dim, &p);

    double t0 = now_s();
    for (size_t i = 0; i < N; i++) hnsw_add(h, i, &vecs[i * dim], dim);
    double build_s = now_s() - t0;

    /* Queries near random cluster centers, so each has a genuine neighbourhood. */
    float *queries = malloc(Q * dim * sizeof(float));
    for (size_t i = 0; i < Q; i++) {
        const float *c = &centers[(g_rng % C) * dim];
        for (size_t d = 0; d < dim; d++)
            queries[i * dim + d] = c[d] + 0.15f * frand();
    }

    uint64_t *truth = malloc(K * sizeof(uint64_t));
    size_t hits = 0;
    double query_s = 0;
    for (size_t qi = 0; qi < Q; qi++) {
        const float *q = &queries[qi * dim];
        brute_top_k(vecs, N, dim, q, K, truth);
        uint64_t *ids = NULL;
        float *sc = NULL;
        size_t n = 0;
        double q0 = now_s();
        hnsw_search(h, q, dim, K, efs, &ids, &sc, &n);
        query_s += now_s() - q0;
        for (size_t i = 0; i < n; i++)
            for (size_t j = 0; j < K; j++)
                if (ids[i] == truth[j]) { hits++; break; }
        free(ids);
        free(sc);
    }

    printf("build:        %.3f s  (%.1f us/insert)\n", build_s,
           build_s * 1e6 / (double)N);
    printf("query:        %.1f us/query (avg over %zu)\n",
           query_s * 1e6 / (double)Q, Q);
    printf("recall@%zu:     %.4f\n", K, (double)hits / (double)(Q * K));

    free(truth);
    free(queries);
    free(vecs);
    free(centers);
    hnsw_free(h);
    return 0;
}