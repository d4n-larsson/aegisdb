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
 * `ef_search` tunes the HNSW query beam (0 = the HNSW default). `quantize`
 * stores the graph's vectors as int8 (~4x smaller, small recall cost). */
SemanticIndex *semantic_index_create(size_t dim, size_t ann_threshold,
                                     size_t ef_search, int quantize);
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

/* Reset to empty, preserving the dim / threshold / ef_search configuration. */
void semantic_index_clear(SemanticIndex *s);

/* Persist the HNSW graph (if one exists) to `path`, tagged with the log offset
 * it reflects, so recovery can load it and replay only the tail. When there is
 * no graph (still below the threshold) any stale checkpoint at `path` is
 * removed. Returns 0 on success, -1 on write failure. */
int semantic_index_save(const SemanticIndex *s, const char *path,
                        uint64_t covered_log_size);

/* Load a graph checkpoint from `path` into a fresh (empty) index and rebuild
 * the authoritative dense array from it. Writes the tagged offset to
 * *out_covered_log_size. Returns 0 on success; -1 (leaving the index empty) if
 * the checkpoint is missing/corrupt/incompatible — the caller then rebuilds. */
int semantic_index_load(SemanticIndex *s, const char *path,
                        uint64_t *out_covered_log_size);

/* Drop every id for which keep(id, ctx) returns 0. Used after a checkpoint load
 * to evict ids deleted in the replayed log tail (which the load cannot see). */
void semantic_index_reconcile(SemanticIndex *s,
                              int (*keep)(uint64_t id, void *ctx), void *ctx);

#endif /* AEGISDB_SEMANTIC_INDEX_H */