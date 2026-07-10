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
 * stores the graph's vectors as int8 (~4x smaller, small recall cost).
 * `shard_target` sets the target vectors per HNSW shard (0 = built-in default):
 * a large graph splits into ~count/target shards, built in parallel. */
SemanticIndex *semantic_index_create(size_t dim, size_t ann_threshold,
                                     size_t ef_search, int quantize,
                                     size_t shard_target);
void semantic_index_free(SemanticIndex *s);

/* Add or replace the `vec_count` vectors for `id` (copied; vector i at
 * vecs + i*dim). Any prior vectors for `id` are replaced. dim must match.
 * Returns 0/-1. */
int semantic_index_add(SemanticIndex *s, uint64_t id, const float *vecs,
                       size_t vec_count, size_t dim);
void semantic_index_remove(SemanticIndex *s, uint64_t id);

/* Top-K record ids by cosine similarity. A multi-vector record is scored by its
 * best-matching vector and returned once (best-of-N). Allocates *out_ids and
 * *out_scores (free each). Returns 0/-1. */
int semantic_index_search(const SemanticIndex *s, const float *query,
                          size_t dim, size_t top_k, uint64_t **out_ids,
                          float **out_scores, size_t *out_n);

/* Number of stored vectors. */
size_t semantic_index_count(const SemanticIndex *s);

/* --- HNSW graph construction --------------------------------------------- */

/* Build the HNSW graph from the current dense vectors synchronously and drop
 * the dense array. Blocks the caller for the whole build, so this is for
 * single-threaded contexts and tests; the server uses the deferred path below.
 * Returns 0 on success, -1 if a graph already exists or on failure. */
int semantic_index_build_now(SemanticIndex *s);

/* Deferred, off-lock HNSW build, mirroring compaction's snapshot/build/swap so
 * the graph construction never holds the index lock. The orchestrator (the
 * maintenance thread) owns the returned job and provides the locking; the
 * comments state which phase each call belongs to:
 *
 *   1. needs_build()  — cheap poll (read lock) for a pending threshold crossing
 *   2. build_begin()  — WRITE lock: snapshot the dense vectors, start journalling
 *   3. build_run()    — NO lock: build the graph from the snapshot
 *   4. catch-up loop  — WRITE lock: take_deltas(); NO lock: apply() into the graph
 *   5. build_commit() — WRITE lock: replay residual deltas + install + drop dense
 *
 * begin() sets the index into "building" mode: add/remove keep updating the
 * dense array (which still answers searches) and additionally journal each op so
 * apply()/commit() can catch the new graph up to writes that raced it. */
typedef struct SemBuildJob SemBuildJob;

/* True when the live count has reached the threshold but no graph exists and no
 * build is in flight. Call under a read lock. */
int semantic_index_needs_build(const SemanticIndex *s);

/* Phase 2 (WRITE lock): snapshot dense vectors, enter building mode. Returns a
 * job to build off-lock, or NULL if a build is not needed / on OOM. */
SemBuildJob *semantic_index_build_begin(SemanticIndex *s);

/* Phase 3 (NO lock): build the graph from the job's snapshot. Returns 0/-1. */
int semantic_index_build_run(SemBuildJob *job);

/* Phase 4a (WRITE lock): move the deltas journalled since begin()/the last take
 * into the job. Returns the number taken (bounds the phase 4b work). */
size_t semantic_index_build_take_deltas(SemanticIndex *s, SemBuildJob *job);

/* Phase 4b (NO lock): replay the deltas the job holds into its graph. Returns
 * 0/-1 (an add may OOM). */
int semantic_index_build_apply(SemBuildJob *job);

/* Whether journalling a delta failed (OOM); the build must be aborted. Call
 * under the write lock. */
int semantic_index_build_failed(const SemanticIndex *s);

/* Phase 5 (WRITE lock): replay any residual deltas, install the graph as the
 * live index and drop the dense array. Consumes the job. Returns 0; on -1 the
 * build is aborted and the dense array stays authoritative. */
int semantic_index_build_commit(SemanticIndex *s, SemBuildJob *job);

/* Abort an in-flight build (WRITE lock), freeing the job; dense stays live. */
void semantic_index_build_abort(SemanticIndex *s, SemBuildJob *job);

/* Reset to empty, preserving the dim / threshold / ef_search configuration. */
void semantic_index_clear(SemanticIndex *s);

/* Persist the HNSW graph (if one exists) to `path`, tagged with the log offset
 * it reflects, so recovery can load it and replay only the tail. When there is
 * no graph (still below the threshold) any stale checkpoint at `path` is
 * removed. Returns 0 on success, -1 on write failure. */
int semantic_index_save(const SemanticIndex *s, const char *path,
                        uint64_t covered_log_size, const uint8_t *key);

/* Load a graph checkpoint from `path` into a fresh (empty) index and rebuild
 * the authoritative dense array from it. Writes the tagged offset to
 * *out_covered_log_size. `key` (or NULL) decrypts the checkpoint. Returns 0 on
 * success; -1 (leaving the index empty) if the checkpoint is
 * missing/corrupt/incompatible or the key is wrong — the caller then rebuilds. */
int semantic_index_load(SemanticIndex *s, const char *path,
                        uint64_t *out_covered_log_size, const uint8_t *key);

/* Drop every id for which keep(id, ctx) returns 0. Used after a checkpoint load
 * to evict ids deleted in the replayed log tail (which the load cannot see). */
void semantic_index_reconcile(SemanticIndex *s,
                              int (*keep)(uint64_t id, void *ctx), void *ctx);

#endif /* AEGISDB_SEMANTIC_INDEX_H */