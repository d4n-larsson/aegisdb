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

#endif /* AEGISDB_VECMATH_H */