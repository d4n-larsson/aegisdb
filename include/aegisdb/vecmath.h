/* Small vector-math helpers shared by the semantic and HNSW indexes. */
#ifndef AEGISDB_VECMATH_H
#define AEGISDB_VECMATH_H

#include <math.h>
#include <stddef.h>

/* Euclidean (L2) norm of a `dim`-dimensional float vector, accumulated in
 * double for precision. */
static inline float l2norm(const float *v, size_t dim) {
    double acc = 0;
    for (size_t i = 0; i < dim; i++) acc += (double)v[i] * v[i];
    return (float)sqrt(acc);
}

/* Dot product of two `dim`-length float vectors.
 *
 * Four independent accumulators break the single serial add-dependency chain a
 * naive reduction creates, so the CPU can pipeline the FMAs and the compiler
 * can pack them into SIMD lanes (8 floats/AVX register vs the 4 doubles a
 * double accumulator allowed). This is the single hottest loop in both the
 * exact cosine scan and the HNSW graph (build + query), so keeping it in float
 * roughly doubles SIMD width; the precision cost is negligible for cosine
 * ranking (recall@k is unchanged in the bench). `restrict` lets the compiler
 * assume the inputs don't alias. */
static inline float dot_f32(const float *restrict a, const float *restrict b,
                            size_t dim) {
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    size_t i = 0;
    for (; i + 4 <= dim; i += 4) {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    float s = (s0 + s1) + (s2 + s3);
    for (; i < dim; i++) s += a[i] * b[i];
    return s;
}

#endif /* AEGISDB_VECMATH_H */