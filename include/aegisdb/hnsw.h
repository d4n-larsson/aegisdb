/* HNSW (Hierarchical Navigable Small World) approximate nearest-neighbour index
 * over float32 vectors in cosine space (#38).
 *
 * Self-contained C17 — no external deps — so it preserves the single-binary
 * goal that ruled out vendoring a C++ HNSW library. This is the standalone
 * module; wiring it behind SemanticIndex (as the large-n path of a hybrid that
 * keeps the exact scan for small n) is a follow-up.
 *
 * Approximate: search returns the top_k by cosine similarity with high recall,
 * not a guaranteed-exact ordering. Insert/search are incremental (no training).
 * Layer assignment uses a seeded PRNG, so a given (seed, insertion order) builds
 * a reproducible graph. */
#ifndef AEGISDB_HNSW_H
#define AEGISDB_HNSW_H

#include <stddef.h>
#include <stdint.h>

typedef struct Hnsw Hnsw;

/* Tunables. Pass 0 for any field to take its default (M=16, ef_construction=200,
 * ef_search=50, seed=0x9E3779B97F4A7C15). M0 (layer-0 degree) is fixed at 2*M. */
typedef struct {
    size_t M;               /* max neighbours per node above layer 0 */
    size_t ef_construction; /* candidate beam width while inserting */
    size_t ef_search;       /* default candidate beam width while querying */
    uint64_t seed;          /* PRNG seed for reproducible layer assignment */
} HnswParams;

Hnsw *hnsw_create(size_t dim, const HnswParams *params);
void hnsw_free(Hnsw *h);

/* Insert the vector for `id` (copied), or replace it if `id` already exists
 * (the previous node is tombstoned and a fresh one inserted). dim must match.
 * Returns 0 on success, -1 on allocation failure or dim mismatch. */
int hnsw_add(Hnsw *h, uint64_t id, const float *vec, size_t dim);

/* Soft-delete `id`: its node is tombstoned (excluded from results) but kept in
 * the graph for connectivity. No-op if absent. */
void hnsw_remove(Hnsw *h, uint64_t id);

/* Top-`top_k` ids by cosine similarity, most-similar first. `ef_search` bounds
 * the search beam (clamped to >= top_k); pass 0 for the index default.
 * Allocates *out_ids and *out_scores (free each); *out_n <= top_k. 0/-1. */
int hnsw_search(const Hnsw *h, const float *query, size_t dim, size_t top_k,
                size_t ef_search, uint64_t **out_ids, float **out_scores,
                size_t *out_n);

/* Number of live (non-tombstoned) vectors. */
size_t hnsw_count(const Hnsw *h);

/* Total nodes including tombstoned ones (>= hnsw_count). Diagnostic: the graph
 * compacts itself once tombstones reach half the array, so this stays within ~2x
 * the live count under churn. */
size_t hnsw_total_nodes(const Hnsw *h);

/* Invoke `cb(id, vec, ctx)` for each live vector (vec is the stored copy, length
 * = the index dim). Stops early and returns cb's value if it returns nonzero;
 * otherwise returns 0. Used to repopulate a caller-side store after a load. */
int hnsw_foreach_live(const Hnsw *h,
                      int (*cb)(uint64_t id, const float *vec, void *ctx),
                      void *ctx);

/* Persist the graph to `path` (atomic .tmp + rename, CRC-protected), tagging it
 * with the log offset it reflects so recovery can replay only the tail. Returns
 * 0/-1. */
int hnsw_save(const Hnsw *h, const char *path, uint64_t covered_log_size);

/* Load a graph previously written by hnsw_save. Returns NULL if the file is
 * missing, truncated/corrupt (bad CRC), a different format version, or built
 * for a different `expected_dim` — the caller then falls back to a full
 * rebuild. On success writes the tagged offset to *out_covered_log_size. */
Hnsw *hnsw_load(const char *path, size_t expected_dim,
                uint64_t *out_covered_log_size);

#endif /* AEGISDB_HNSW_H */