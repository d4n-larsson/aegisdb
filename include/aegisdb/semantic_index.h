/* Semantic similarity index (T035).
 *
 * Stores float32 embedding vectors and answers top-K nearest-neighbour queries
 * by cosine similarity. The contract (research R-008) names HNSW as the target
 * structure; vendoring a C++ HNSW library conflicts with the single-C17-binary
 * goal, so this is a self-contained exact cosine scan — functionally correct
 * and a drop-in to be replaced by an HNSW graph for the SC-006 scale target. */
#ifndef AEGISDB_SEMANTIC_INDEX_H
#define AEGISDB_SEMANTIC_INDEX_H

#include <stddef.h>
#include <stdint.h>

typedef struct SemanticIndex SemanticIndex;

/* Create a semantic index over `dim`-length vectors. Search is exact while the
 * live count is below `ann_threshold` (0 = a built-in default); above it the
 * index builds and queries an HNSW graph for sublinear approximate top-K.
 * `ef_search` tunes the HNSW query beam (0 = the HNSW default). */
SemanticIndex *semantic_index_create(size_t dim, size_t ann_threshold,
                                     size_t ef_search);
void semantic_index_free(SemanticIndex *s);

/* Add or replace the vector for `id` (copied). dim must match. Returns 0/-1. */
int semantic_index_add(SemanticIndex *s, uint64_t id, const float *vec,
                       size_t dim);
void semantic_index_remove(SemanticIndex *s, uint64_t id);

/* Top-K by cosine similarity. Allocates *out_ids and *out_scores (free each).
 * Returns 0/-1. */
int semantic_index_search(const SemanticIndex *s, const float *query,
                          size_t dim, size_t top_k, uint64_t **out_ids,
                          float **out_scores, size_t *out_n);

/* Number of stored vectors. */
size_t semantic_index_count(const SemanticIndex *s);

#endif /* AEGISDB_SEMANTIC_INDEX_H */