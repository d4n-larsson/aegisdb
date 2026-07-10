/* Semantic index (T035, #38): hybrid cosine top-K over stored float32 vectors.
 *
 * Vectors live in a dense array `e[0..n)`; an open-addressing `id -> slot` map
 * keeps add / lookup / remove O(1) so building or recovering an index of N
 * vectors is O(N), not O(N^2). The map stores the dense index, so a swap-remove
 * only has to repoint the one moved entry.
 *
 * Search is exact (brute-force scan over the dense array) while small. Once the
 * live count crosses `ann_threshold`, an HNSW graph is built from the dense
 * array and the dense array is freed: the graph then becomes the sole store
 * (one copy of each vector) and serves add/remove/search/count. The switch is
 * one-way — a later dip below the threshold keeps using the graph. */
#include "aegisdb/semantic_index.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/hash_mix.h"
#include "aegisdb/hnsw.h"
#include "aegisdb/vecmath.h"

#define DEFAULT_ANN_THRESHOLD 10000 /* live vectors above which HNSW kicks in */

/* Sharding (#parallel-build): at scale the HNSW graph is split into independent
 * shards, each an ordinary single-threaded HNSW over a hash-partitioned subset
 * of the vectors. The shards share no state, so the (expensive) build runs one
 * thread per shard for a near-linear speedup, and add/remove/search route by the
 * same hash. A vector's shard is stable: mix64(synthetic id) % nshards. Sharding
 * only kicks in for large indexes — small ones stay a single graph (nshards==1),
 * unchanged, so search fans out over at most a handful of shards. */
#define SHARD_TARGET 25000  /* aim for ~this many vectors per shard */
#define SHARD_MAX 8          /* also capped by online CPUs (build parallelism) */

/* Resolve the effective per-shard vector target. Precedence: the configured
 * value (--ann-shard-target, passed through here) wins; then AEGIS_SHARD_TARGET
 * (a build-time-only env read — test seam and ops escape hatch); then the
 * built-in default. */
static size_t resolve_shard_target(size_t configured) {
    if (configured > 0) return configured;
    const char *e = getenv("AEGIS_SHARD_TARGET");
    if (e) {
        long v = strtol(e, NULL, 10);
        if (v > 0) return (size_t)v;
    }
    return SHARD_TARGET;
}

/* Number of shards for a build of `n` vectors: 1 below ~2x the per-shard target,
 * then ~n/target, capped by SHARD_MAX and the online CPU count (one build thread
 * per shard). Deterministic given n + cores. */
static size_t pick_nshards(size_t n, size_t configured_target) {
    size_t by_size = n / resolve_shard_target(configured_target); /* 0 for n < target */
    if (by_size < 2) return 1;           /* single graph for small indexes */
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    size_t cap = SHARD_MAX;
    if (cpus > 0 && (size_t)cpus < cap) cap = (size_t)cpus;
    return by_size < cap ? by_size : cap;
}

/* Which shard a synthetic id belongs to (stable across build/add/remove/search). */
static size_t shard_index(uint64_t synid, size_t nshards) {
    return nshards > 1 ? (size_t)(mix64(synid) % nshards) : 0;
}

/* Multi-vector (#85): a record's N vectors are stored as N separate entries
 * keyed by a synthetic id `record << MV_SLOT_BITS | slot`, so the dense map and
 * HNSW (both keyed by an opaque uint64) stay 1:1 per vector. Search maps the
 * synthetic ids back to record ids and returns each record once (best-of-N).
 * MV_SLOT_BITS caps a record at 64 vectors (matches the wire cap). */
#define MV_SLOT_BITS 6
#define MV_SLOT_MASK ((1u << MV_SLOT_BITS) - 1u)
static uint64_t mv_syn(uint64_t rec, size_t slot) {
    return (rec << MV_SLOT_BITS) | (uint64_t)slot;
}
static uint64_t mv_rec(uint64_t syn) { return syn >> MV_SLOT_BITS; }

typedef struct {
    uint64_t id;
    float *vec;
    float norm; /* precomputed L2 norm */
    int used;
} SemEntry;

typedef struct {
    uint64_t id;
    size_t idx; /* position of this id in SemanticIndex.e */
    int used;
} MapSlot;

/* record id -> number of vectors it currently has (so remove/replace know how
 * many synthetic slots to drop). Open addressing with backward-shift deletion. */
typedef struct {
    uint64_t rec;
    uint32_t count;
    int used;
} RCount;

/* One journalled mutation during a deferred build: `vec` non-NULL is an add
 * (owns a copy of the vector), NULL is a remove. Replayed in order into the
 * newly built graph so it converges to the live dense state. */
typedef struct {
    uint64_t synid;
    float *vec;
} SemDelta;

/* An in-flight off-lock graph build. Snapshot taken under the write lock, the
 * shards built with no lock held (one thread each), then installed under the
 * write lock. */
struct SemBuildJob {
    size_t dim;
    size_t n;          /* snapshot vector count */
    uint64_t *synids;  /* snapshot synthetic ids (n) */
    float *vecs;       /* snapshot vectors, n * dim contiguous */
    size_t ef_search;
    int quantize;
    Hnsw **shards;     /* nshards graphs built in phase 3 */
    size_t nshards;
    SemDelta *pending; /* deltas taken from the index, awaiting replay */
    size_t npending;
};

struct SemanticIndex {
    size_t dim;
    SemEntry *e;
    size_t n;
    size_t cap;
    MapSlot *map; /* id -> dense slot; power-of-two capacity, NULL until first add */
    size_t mcap;
    RCount *rc;   /* record -> vec_count; power-of-two capacity */
    size_t rccap, rcn;
    Hnsw **shards;       /* NULL/0 until the live count crosses ann_threshold, */
    size_t nshards;      /* then nshards>=1 graphs, hash-partitioned by synid */
    size_t ann_threshold;
    size_t ef_search;    /* HNSW query beam width; 0 = the HNSW default */
    int quantize;        /* store HNSW vectors as int8 (#75) */
    size_t shard_target; /* configured vectors/shard (--ann-shard-target); 0 = default */

    /* Deferred off-lock graph build (see semantic_index_build_*). While a build
     * runs on the maintenance thread, the dense array stays authoritative for
     * search, and every add/remove is also journalled here so the freshly built
     * graph can be caught up to the live state before it is installed. */
    int building;        /* a build_begin() is outstanding: journal deltas */
    int build_failed;    /* a delta could not be journalled (OOM): abort at commit */
    SemDelta *deltas;    /* journalled add/remove ops since build_begin() */
    size_t ndelta, deltacap;
};

#define MAP_INITIAL_CAP 128 /* power of two; covers the dense array's first growths */

static void deltas_free(SemDelta *d, size_t n); /* defined with the build API */

/* Probe to the slot holding `id`, or the empty slot where it would go. Requires
 * mcap > 0 and at least one empty slot (the load factor guarantees both). */
static size_t map_probe(const MapSlot *map, size_t mcap, uint64_t id) {
    size_t mask = mcap - 1;
    size_t i = (size_t)mix64(id) & mask;
    while (map[i].used && map[i].id != id) i = (i + 1) & mask;
    return i;
}

/* Rebuild the map at `newcap` from the dense array (the dense indices are the
 * source of truth, so this also resolves any prior probe-chain layout). */
static int map_grow(SemanticIndex *s, size_t newcap) {
    MapSlot *nm = calloc(newcap, sizeof(MapSlot));
    if (!nm) return -1;
    size_t mask = newcap - 1;
    for (size_t i = 0; i < s->n; i++) {
        size_t j = (size_t)mix64(s->e[i].id) & mask;
        while (nm[j].used) j = (j + 1) & mask;
        nm[j].used = 1;
        nm[j].id = s->e[i].id;
        nm[j].idx = i;
    }
    free(s->map);
    s->map = nm;
    s->mcap = newcap;
    return 0;
}

/* Backward-shift deletion: remove the entry at slot `i`, sliding later members
 * of its probe chain back so no tombstones are left and probes stay correct. */
static void map_delete_at(SemanticIndex *s, size_t i) {
    MapSlot *m = s->map;
    size_t mask = s->mcap - 1;
    size_t j = i;
    for (;;) {
        m[i].used = 0;
        size_t k;
        do {
            j = (j + 1) & mask;
            if (!m[j].used) return;
            k = (size_t)mix64(m[j].id) & mask; /* home slot of m[j] */
        } while (i <= j ? (i < k && k <= j) : (i < k || k <= j));
        m[i] = m[j]; /* moves id+idx together; used stays 1 */
        i = j;
    }
}

/* ----- record -> vec_count map (rc) ------------------------------------- */
static size_t rc_probe(const RCount *m, size_t cap, uint64_t rec) {
    size_t mask = cap - 1, i = (size_t)mix64(rec) & mask;
    while (m[i].used && m[i].rec != rec) i = (i + 1) & mask;
    return i;
}
static int rc_grow(SemanticIndex *s, size_t newcap) {
    RCount *nm = calloc(newcap, sizeof(RCount));
    if (!nm) return -1;
    size_t mask = newcap - 1;
    for (size_t i = 0; i < s->rccap; i++) {
        if (!s->rc[i].used) continue;
        size_t j = (size_t)mix64(s->rc[i].rec) & mask;
        while (nm[j].used) j = (j + 1) & mask;
        nm[j] = s->rc[i];
    }
    free(s->rc);
    s->rc = nm;
    s->rccap = newcap;
    return 0;
}
static uint32_t rc_get(const SemanticIndex *s, uint64_t rec) {
    if (s->rccap == 0) return 0;
    size_t i = rc_probe(s->rc, s->rccap, rec);
    return s->rc[i].used ? s->rc[i].count : 0;
}
static int rc_set(SemanticIndex *s, uint64_t rec, uint32_t count) {
    if (s->rccap == 0 && rc_grow(s, 64) != 0) return -1;
    size_t i = rc_probe(s->rc, s->rccap, rec);
    if (!s->rc[i].used) {
        if ((s->rcn + 1) * 10 > s->rccap * 7) {
            if (rc_grow(s, s->rccap * 2) != 0) return -1;
            i = rc_probe(s->rc, s->rccap, rec);
        }
        s->rc[i].used = 1;
        s->rc[i].rec = rec;
        s->rcn++;
    }
    s->rc[i].count = count;
    return 0;
}
static void rc_del(SemanticIndex *s, uint64_t rec) {
    if (s->rccap == 0) return;
    size_t mask = s->rccap - 1;
    size_t i = rc_probe(s->rc, s->rccap, rec);
    if (!s->rc[i].used) return;
    s->rc[i].used = 0;
    s->rcn--;
    for (size_t j = (i + 1) & mask; s->rc[j].used; j = (j + 1) & mask) {
        size_t home = (size_t)mix64(s->rc[j].rec) & mask;
        if (i <= j ? (i < home && home <= j) : (i < home || home <= j)) continue;
        s->rc[i] = s->rc[j];
        s->rc[j].used = 0;
        i = j;
    }
}

SemanticIndex *semantic_index_create(size_t dim, size_t ann_threshold,
                                     size_t ef_search, int quantize,
                                     size_t shard_target) {
    SemanticIndex *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->dim = dim;
    s->ann_threshold = ann_threshold ? ann_threshold : DEFAULT_ANN_THRESHOLD;
    s->ef_search = ef_search;
    s->quantize = quantize;
    s->shard_target = shard_target; /* 0 -> resolved to env / built-in default */
    return s;
}

/* True once the graph (one or more shards) is authoritative and the dense array
 * has been dropped. */
static int graph_present(const SemanticIndex *s) { return s->nshards > 0; }

/* Total live vectors across all shards. */
static size_t graph_count(const SemanticIndex *s) {
    size_t total = 0;
    for (size_t i = 0; i < s->nshards; i++) total += hnsw_count(s->shards[i]);
    return total;
}

/* Free an array of `n` shard graphs and the array itself. */
static void shards_free(Hnsw **shards, size_t n) {
    for (size_t i = 0; i < n; i++) hnsw_free(shards[i]);
    free(shards);
}

size_t semantic_index_count(const SemanticIndex *s) {
    if (!s) return 0;
    /* Above the threshold the graph is authoritative; the dense array is freed. */
    return graph_present(s) ? graph_count(s) : s->n;
}

/* Free the dense array + id-map, keeping the HNSW graph. Used once the graph
 * becomes authoritative so each vector is stored once (in the graph). */
static void drop_dense(SemanticIndex *s) {
    for (size_t i = 0; i < s->n; i++) free(s->e[i].vec);
    free(s->e);
    s->e = NULL;
    s->n = s->cap = 0;
    free(s->map);
    s->map = NULL;
    s->mcap = 0;
}

void semantic_index_free(SemanticIndex *s) {
    if (!s) return;
    semantic_index_clear(s);
    free(s);
}

/* Reset to empty, preserving dim/ann_threshold/ef_search. */
void semantic_index_clear(SemanticIndex *s) {
    if (!s) return;
    for (size_t i = 0; i < s->n; i++) free(s->e[i].vec);
    free(s->e);
    s->e = NULL;
    s->n = s->cap = 0;
    free(s->map);
    s->map = NULL;
    s->mcap = 0;
    free(s->rc);
    s->rc = NULL;
    s->rccap = s->rcn = 0;
    shards_free(s->shards, s->nshards);
    s->shards = NULL;
    s->nshards = 0;
    deltas_free(s->deltas, s->ndelta);
    s->deltas = NULL;
    s->ndelta = s->deltacap = 0;
    s->building = 0;
    s->build_failed = 0;
}

#define HNSW_BUILD_SEED 0x9E3779B97F4A7C15ULL

/* Build the HNSW graph from the current dense array, then drop the dense array
 * so the graph holds the only copy of each vector; thereafter add/remove
 * operate on the graph. Synchronous — blocks the caller for the whole build, so
 * the server uses the deferred off-lock path below; this stays for
 * single-threaded contexts and tests. Returns 0/-1. */
int semantic_index_build_now(SemanticIndex *s) {
    if (!s || graph_present(s)) return -1;
    HnswParams p = {.ef_search = s->ef_search, .seed = HNSW_BUILD_SEED,
                    .quantize = s->quantize};
    Hnsw *h = hnsw_create(s->dim, &p);
    if (!h) return -1;
    for (size_t i = 0; i < s->n; i++) {
        if (hnsw_add(h, s->e[i].id, s->e[i].vec, s->dim) != 0) {
            hnsw_free(h);
            return -1;
        }
    }
    Hnsw **shards = malloc(sizeof(Hnsw *));
    if (!shards) {
        hnsw_free(h);
        return -1;
    }
    shards[0] = h; /* synchronous path is always a single shard */
    s->shards = shards;
    s->nshards = 1;
    drop_dense(s);
    return 0;
}

/* ---------------------------------------------- deferred off-lock build ---- */
/* The graph is expensive to build (~seconds at production scale). Building it
 * inline under the index write lock froze every reader and writer for the whole
 * build. Instead the maintenance thread drives a three-phase build that mirrors
 * compaction: snapshot under a brief write lock (begin), build with no lock held
 * (run), then install under a brief write lock (commit), catching the new graph
 * up to writes that raced the build via a journal of deltas. */

/* Free a delta array (and the vector copies it owns). */
static void deltas_free(SemDelta *d, size_t n) {
    for (size_t i = 0; i < n; i++) free(d[i].vec);
    free(d);
}

int semantic_index_needs_build(const SemanticIndex *s) {
    return s && !graph_present(s) && !s->building && s->n >= s->ann_threshold;
}

SemBuildJob *semantic_index_build_begin(SemanticIndex *s) {
    if (!semantic_index_needs_build(s)) return NULL;
    SemBuildJob *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->dim = s->dim;
    j->n = s->n;
    j->nshards = pick_nshards(s->n, s->shard_target); /* fixed for this graph's life */
    j->ef_search = s->ef_search;
    j->quantize = s->quantize;
    j->synids = malloc(s->n * sizeof(uint64_t));
    j->vecs = malloc(s->n * s->dim * sizeof(float));
    if (!j->synids || !j->vecs) {
        free(j->synids);
        free(j->vecs);
        free(j);
        return NULL;
    }
    for (size_t i = 0; i < s->n; i++) {
        j->synids[i] = s->e[i].id;
        memcpy(j->vecs + i * s->dim, s->e[i].vec, s->dim * sizeof(float));
    }
    s->building = 1;
    s->build_failed = 0;
    return j;
}

/* One shard's worth of build: an ordinary single-threaded HNSW over just the
 * snapshot vectors hashed to `shard`. Shards share no state, so these run
 * concurrently (one pthread each). */
struct shard_work {
    const SemBuildJob *job;
    size_t shard;
    Hnsw *out; /* result graph, or NULL on failure */
};

static void *build_shard_worker(void *arg) {
    struct shard_work *w = arg;
    const SemBuildJob *j = w->job;
    HnswParams p = {.ef_search = j->ef_search, .seed = HNSW_BUILD_SEED,
                    .quantize = j->quantize};
    Hnsw *h = hnsw_create(j->dim, &p);
    if (!h) {
        w->out = NULL;
        return NULL;
    }
    for (size_t i = 0; i < j->n; i++) {
        if (shard_index(j->synids[i], j->nshards) != w->shard) continue;
        if (hnsw_add(h, j->synids[i], j->vecs + i * j->dim, j->dim) != 0) {
            hnsw_free(h);
            w->out = NULL;
            return NULL;
        }
    }
    w->out = h;
    return NULL;
}

int semantic_index_build_run(SemBuildJob *job) {
    if (!job) return -1;
    size_t P = job->nshards ? job->nshards : 1;
    Hnsw **shards = calloc(P, sizeof(Hnsw *));
    struct shard_work *work = calloc(P, sizeof(*work));
    pthread_t *tids = calloc(P, sizeof(pthread_t));
    if (!shards || !work || !tids) {
        free(shards);
        free(work);
        free(tids);
        return -1;
    }
    for (size_t i = 0; i < P; i++) {
        work[i].job = job;
        work[i].shard = i;
    }
    /* Spawn one thread per shard; any that fail to start (or the single-shard
     * case) run inline on this thread, so the build always completes. */
    size_t started = 0;
    if (P > 1) {
        for (size_t i = 0; i < P; i++) {
            if (pthread_create(&tids[i], NULL, build_shard_worker, &work[i]) != 0)
                break;
            started++;
        }
    }
    for (size_t i = started; i < P; i++) build_shard_worker(&work[i]);
    for (size_t i = 0; i < started; i++) pthread_join(tids[i], NULL);

    int ok = 1;
    for (size_t i = 0; i < P; i++) {
        shards[i] = work[i].out;
        if (!shards[i]) ok = 0;
    }
    free(work);
    free(tids);
    if (!ok) {
        shards_free(shards, P);
        return -1;
    }
    job->shards = shards;
    return 0;
}

size_t semantic_index_build_take_deltas(SemanticIndex *s, SemBuildJob *job) {
    /* Consume any deltas the job holds but has not replayed (shouldn't happen —
     * apply() clears them — but stay leak-free). */
    deltas_free(job->pending, job->npending);
    job->pending = s->deltas;
    job->npending = s->ndelta;
    s->deltas = NULL;
    s->ndelta = s->deltacap = 0;
    return job->npending;
}

int semantic_index_build_failed(const SemanticIndex *s) {
    return s->build_failed;
}

int semantic_index_build_apply(SemBuildJob *job) {
    int rc = 0;
    for (size_t i = 0; i < job->npending; i++) {
        const SemDelta *d = &job->pending[i];
        Hnsw *g = job->shards[shard_index(d->synid, job->nshards)];
        if (d->vec) {
            if (hnsw_add(g, d->synid, d->vec, job->dim) != 0) rc = -1;
        } else {
            hnsw_remove(g, d->synid);
        }
    }
    deltas_free(job->pending, job->npending);
    job->pending = NULL;
    job->npending = 0;
    return rc;
}

/* Free a job and everything it owns. */
static void build_job_free(SemBuildJob *job) {
    if (!job) return;
    shards_free(job->shards, job->nshards);
    deltas_free(job->pending, job->npending);
    free(job->synids);
    free(job->vecs);
    free(job);
}

void semantic_index_build_abort(SemanticIndex *s, SemBuildJob *job) {
    s->building = 0;
    s->build_failed = 0;
    /* Drop the deltas journalled during this attempt; the dense array already
     * reflects them, so it stays authoritative and a later begin() re-snapshots. */
    deltas_free(s->deltas, s->ndelta);
    s->deltas = NULL;
    s->ndelta = s->deltacap = 0;
    build_job_free(job);
}

int semantic_index_build_commit(SemanticIndex *s, SemBuildJob *job) {
    /* Replay any residual deltas journalled since the last take (kept small by
     * the caller's catch-up loop), then swap the graph in atomically. */
    if (s->build_failed) {
        semantic_index_build_abort(s, job);
        return -1;
    }
    semantic_index_build_take_deltas(s, job);
    if (semantic_index_build_apply(job) != 0) {
        semantic_index_build_abort(s, job);
        return -1;
    }
    s->shards = job->shards;
    s->nshards = job->nshards;
    job->shards = NULL; /* ownership transferred; keep build_job_free from freeing */
    job->nshards = 0;
    s->building = 0;
    drop_dense(s);
    build_job_free(job);
    return 0;
}

/* Insert or replace id's vector in the dense array + id-map only (no graph).
 * Returns 1 if a new entry was added, 0 if an existing one was replaced in
 * place, -1 on allocation failure. */
static int dense_put(SemanticIndex *s, uint64_t id, const float *vec,
                     size_t dim) {
    if (s->mcap == 0 && map_grow(s, MAP_INITIAL_CAP) != 0) return -1;

    size_t j = map_probe(s->map, s->mcap, id);
    if (s->map[j].used) {
        SemEntry *e = &s->e[s->map[j].idx];
        float *nv = realloc(e->vec, dim * sizeof(float));
        if (!nv) return -1;
        memcpy(nv, vec, dim * sizeof(float));
        e->vec = nv;
        e->norm = l2norm(vec, dim);
        return 0;
    }

    /* New id. Copy the vector first so an allocation failure changes nothing. */
    float *nv = malloc(dim * sizeof(float));
    if (!nv) return -1;
    memcpy(nv, vec, dim * sizeof(float));

    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        SemEntry *ne = realloc(s->e, cap * sizeof(SemEntry));
        if (!ne) {
            free(nv);
            return -1;
        }
        s->e = ne;
        s->cap = cap;
    }
    /* Keep the map under a ~0.7 load factor; re-probe after a resize. */
    if ((s->n + 1) * 10 > s->mcap * 7) {
        if (map_grow(s, s->mcap * 2) != 0) {
            free(nv);
            return -1;
        }
        j = map_probe(s->map, s->mcap, id);
    }

    size_t idx = s->n++;
    s->e[idx].id = id;
    s->e[idx].vec = nv;
    s->e[idx].norm = l2norm(vec, dim);
    s->e[idx].used = 1;
    s->map[j].used = 1;
    s->map[j].id = id;
    s->map[j].idx = idx;
    return 1;
}

/* Remove one synthetic-id vector from the dense array (caller ensures no HNSW). */
static void dense_remove(SemanticIndex *s, uint64_t synid) {
    if (s->mcap == 0) return;
    size_t j = map_probe(s->map, s->mcap, synid);
    if (!s->map[j].used) return;
    size_t idx = s->map[j].idx;
    map_delete_at(s, j);

    free(s->e[idx].vec);
    /* swap-remove: move the last entry into the hole so the array stays dense
     * and search stays O(live entries); repoint the moved id's map slot. */
    size_t last = s->n - 1;
    if (idx != last) {
        s->e[idx] = s->e[last];
        size_t mj = map_probe(s->map, s->mcap, s->e[idx].id);
        s->map[mj].idx = idx;
    }
    s->n--;
}

/* Journal one mutation while a deferred build is in flight. `vec` non-NULL is
 * an add (copied), NULL a remove. On allocation failure the build is marked
 * failed (aborted at commit) but the dense mutation itself still proceeds, so
 * the index stays correct. */
static void delta_push(SemanticIndex *s, uint64_t synid, const float *vec) {
    if (s->ndelta == s->deltacap) {
        size_t nc = s->deltacap ? s->deltacap * 2 : 64;
        SemDelta *nd = realloc(s->deltas, nc * sizeof(*nd));
        if (!nd) { s->build_failed = 1; return; }
        s->deltas = nd;
        s->deltacap = nc;
    }
    float *copy = NULL;
    if (vec) {
        copy = malloc(s->dim * sizeof(float));
        if (!copy) { s->build_failed = 1; return; }
        memcpy(copy, vec, s->dim * sizeof(float));
    }
    s->deltas[s->ndelta++] = (SemDelta){synid, copy};
}

/* Add/remove one vector by synthetic id, routing to the graph or dense array.
 * When a build is in flight (no graph yet, but one is being constructed) the op
 * also goes to the dense array *and* is journalled so the new graph catches up. */
static int vec_add(SemanticIndex *s, uint64_t synid, const float *vec) {
    if (graph_present(s)) {
        Hnsw *g = s->shards[shard_index(synid, s->nshards)];
        return hnsw_add(g, synid, vec, s->dim) == 0 ? 0 : -1;
    }
    if (dense_put(s, synid, vec, s->dim) < 0) return -1;
    if (s->building) delta_push(s, synid, vec);
    return 0;
}
static void vec_remove(SemanticIndex *s, uint64_t synid) {
    if (graph_present(s)) {
        hnsw_remove(s->shards[shard_index(synid, s->nshards)], synid);
        return;
    }
    dense_remove(s, synid);
    if (s->building) delta_push(s, synid, NULL);
}

int semantic_index_add(SemanticIndex *s, uint64_t id, const float *vecs,
                       size_t vec_count, size_t dim) {
    if (dim != s->dim || vec_count == 0 || vec_count > (MV_SLOT_MASK + 1u))
        return -1;
    /* replace: drop the record's existing vectors (its prior slots) */
    uint32_t old = rc_get(s, id);
    for (uint32_t slot = 0; slot < old; slot++) vec_remove(s, mv_syn(id, slot));

    for (size_t slot = 0; slot < vec_count; slot++)
        if (vec_add(s, mv_syn(id, slot), vecs + slot * dim) != 0) return -1;
    if (rc_set(s, id, (uint32_t)vec_count) != 0) return -1;

    /* Crossing ann_threshold no longer builds the graph inline — that froze
     * every reader and writer for the whole build. The build is now deferred
     * (semantic_index_needs_build + the phased build_* API, driven off-lock by
     * the maintenance thread); the dense array answers searches until it lands. */
    return 0;
}

void semantic_index_remove(SemanticIndex *s, uint64_t id) {
    uint32_t old = rc_get(s, id);
    for (uint32_t slot = 0; slot < old; slot++) vec_remove(s, mv_syn(id, slot));
    rc_del(s, id);
}

typedef struct {
    uint64_t id;
    float score;
} Scored;

static int cmp_scored_desc(const void *a, const void *b) {
    float fa = ((const Scored *)a)->score;
    float fb = ((const Scored *)b)->score;
    if (fa < fb) return 1;
    if (fa > fb) return -1;
    return 0;
}

/* Sift element i down a heap of size n ordered so the root is the LOWEST score
 * (the next to evict): a size-k heap built this way retains the k top scores. */
static void scored_sift_down(Scored *a, size_t n, size_t i) {
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, low = i;
        if (l < n && a[l].score < a[low].score) low = l;
        if (r < n && a[r].score < a[low].score) low = r;
        if (low == i) break;
        Scored t = a[i];
        a[i] = a[low];
        a[low] = t;
        i = low;
    }
}

/* Collapse (record, score) candidates (which may repeat a record across its
 * vectors) to the top_k distinct records by best score. `cand[i].id` is a
 * record id. top_k == 0 returns all distinct records. Allocates the outputs. */
typedef struct { uint64_t rec; size_t idx; int used; } DedupSlot;
static int dedup_topk(Scored *cand, size_t m, size_t top_k, uint64_t **out_ids,
                      float **out_scores, size_t *out_n) {
    size_t cap = 16;
    while (cap < m * 2) cap *= 2;
    DedupSlot *ds = calloc(cap, sizeof(DedupSlot));
    Scored *uni = malloc((m ? m : 1) * sizeof(Scored));
    if (!ds || !uni) {
        free(ds);
        free(uni);
        return -1;
    }
    size_t un = 0, mask = cap - 1;
    for (size_t i = 0; i < m; i++) {
        uint64_t rec = cand[i].id;
        size_t h = mix64(rec) & mask;
        while (ds[h].used && ds[h].rec != rec) h = (h + 1) & mask;
        if (ds[h].used) {
            if (cand[i].score > uni[ds[h].idx].score)
                uni[ds[h].idx].score = cand[i].score; /* best-of-N */
        } else {
            ds[h].used = 1;
            ds[h].rec = rec;
            ds[h].idx = un;
            uni[un].id = rec;
            uni[un].score = cand[i].score;
            un++;
        }
    }
    free(ds);

    size_t k = (top_k && top_k < un) ? top_k : un;
    if (k < un) {
        for (size_t i = k / 2; i-- > 0;) scored_sift_down(uni, k, i);
        for (size_t i = k; i < un; i++)
            if (uni[i].score > uni[0].score) {
                uni[0] = uni[i];
                scored_sift_down(uni, k, 0);
            }
    }
    qsort(uni, k, sizeof(Scored), cmp_scored_desc);
    uint64_t *ids = malloc((k ? k : 1) * sizeof(uint64_t));
    float *sc = malloc((k ? k : 1) * sizeof(float));
    if (!ids || !sc) {
        free(ids);
        free(sc);
        free(uni);
        return -1;
    }
    for (size_t i = 0; i < k; i++) {
        ids[i] = uni[i].id;
        sc[i] = uni[i].score;
    }
    free(uni);
    *out_ids = ids;
    *out_scores = sc;
    *out_n = k;
    return 0;
}

int semantic_index_search(const SemanticIndex *s, const float *query,
                          size_t dim, size_t top_k, uint64_t **out_ids,
                          float **out_scores, size_t *out_n) {
    if (dim != s->dim) return -1;

    if (graph_present(s)) {
        /* Fetch the top candidates from every shard and merge. Fetching the
         * global `want` from each shard is exact for the global top-`want`: a
         * global top-k item has fewer than k items closer to the query, hence
         * fewer than k closer within its own shard, so it is among that shard's
         * top-k. Over-fetch x2 per shard so record-level best-of-N dedup still
         * leaves enough distinct records; then collapse best-of-N. */
        size_t want = top_k ? top_k : graph_count(s);
        if (want == 0) {
            *out_ids = malloc(sizeof(uint64_t));
            *out_scores = malloc(sizeof(float));
            *out_n = 0;
            return (*out_ids && *out_scores) ? 0 : -1;
        }
        size_t per = want * 2;
        size_t cap = 0;
        for (size_t si = 0; si < s->nshards; si++) {
            size_t live = hnsw_count(s->shards[si]);
            cap += per < live ? per : live;
        }
        Scored *cand = malloc((cap ? cap : 1) * sizeof(Scored));
        if (!cand) return -1;
        size_t cn = 0;
        for (size_t si = 0; si < s->nshards; si++) {
            size_t live = hnsw_count(s->shards[si]);
            size_t fetch = per < live ? per : live;
            if (fetch == 0) continue;
            uint64_t *sid = NULL;
            float *ssc = NULL;
            size_t sn = 0;
            if (hnsw_search(s->shards[si], query, dim, fetch, s->ef_search, &sid,
                            &ssc, &sn) != 0) {
                free(cand);
                return -1;
            }
            for (size_t i = 0; i < sn && cn < cap; i++) {
                cand[cn].id = mv_rec(sid[i]);
                cand[cn].score = ssc[i];
                cn++;
            }
            free(sid);
            free(ssc);
        }
        int rc = dedup_topk(cand, cn, top_k, out_ids, out_scores, out_n);
        free(cand);
        return rc;
    }

    /* dense/exact: score every stored vector, then collapse best-of-N */
    float qnorm = l2norm(query, dim);
    Scored *cand = malloc((s->n ? s->n : 1) * sizeof(Scored));
    if (!cand) return -1;
    size_t m = 0;
    for (size_t i = 0; i < s->n; i++) {
        if (!s->e[i].used) continue;
        float dot = dot_f32(s->e[i].vec, query, dim);
        float denom = s->e[i].norm * qnorm;
        cand[m].id = mv_rec(s->e[i].id);
        cand[m].score = denom > 0 ? (float)(dot / denom) : 0.0f;
        m++;
    }
    int rc = dedup_topk(cand, m, top_k, out_ids, out_scores, out_n);
    free(cand);
    return rc;
}
/* ----------------------------------------------------------- persistence -- */

int semantic_index_save(const SemanticIndex *s, const char *path,
                        uint64_t covered_log_size, const uint8_t *key) {
    if (s->nshards == 1)
        return hnsw_save(s->shards[0], path, covered_log_size, key);
    /* No graph yet, or a multi-shard graph: the checkpoint format holds a single
     * graph, so a sharded index is not checkpointed — recovery rebuilds it from
     * the log, which is fast now that the build is parallel. Drop any stale file
     * so recovery does not trust it. */
    unlink(path);
    return 0;
}

typedef struct {
    SemanticIndex *s;
    int err;
} RcRebuild;
/* Called per live graph node on load: bump the record's vec_count to cover this
 * slot (slots are contiguous 0..count-1, so max slot + 1 is the count). */
static int rc_rebuild_cb(uint64_t synid, const float *vec, void *ctx) {
    (void)vec;
    RcRebuild *c = ctx;
    uint64_t rec = mv_rec(synid);
    uint32_t slot = (uint32_t)(synid & MV_SLOT_MASK);
    uint32_t cur = rc_get(c->s, rec);
    if (slot + 1 > cur && rc_set(c->s, rec, slot + 1) != 0) {
        c->err = 1;
        return 1;
    }
    return 0;
}

int semantic_index_load(SemanticIndex *s, const char *path,
                        uint64_t *out_covered_log_size, const uint8_t *key) {
    if (graph_present(s) || s->n != 0) return -1; /* must be a fresh index */
    Hnsw *g = hnsw_load(path, s->dim, out_covered_log_size, key);
    if (!g) return -1;
    /* A checkpoint saved in a different quantization mode than we're configured
     * for is rejected, so recovery rebuilds it in the configured mode. */
    if (hnsw_is_quantized(g) != (s->quantize ? 1 : 0)) {
        hnsw_free(g);
        return -1;
    }
    /* Checkpoints hold a single graph (see save); load it as a one-shard index. */
    Hnsw **shards = malloc(sizeof(Hnsw *));
    if (!shards) {
        hnsw_free(g);
        return -1;
    }
    shards[0] = g;
    s->shards = shards;
    s->nshards = 1; /* the graph is authoritative; no dense array to rebuild */
    /* Rebuild the record->vec_count map from the graph's synthetic node ids
     * (record = id >> MV_SLOT_BITS; slot = id & mask); count = max slot + 1. */
    RcRebuild cx = {s, 0};
    hnsw_foreach_live(g, rc_rebuild_cb, &cx);
    if (cx.err) {
        semantic_index_clear(s);
        return -1;
    }
    return 0;
}

void semantic_index_reconcile(SemanticIndex *s,
                              int (*keep)(uint64_t id, void *ctx), void *ctx) {
    /* The rc map holds exactly the live record ids (one entry per record,
     * regardless of vector count). Snapshot them, then remove those not kept —
     * gathering first so removal doesn't disturb the map we're iterating. */
    uint64_t *recs = malloc((s->rcn ? s->rcn : 1) * sizeof(uint64_t));
    if (!recs) return;
    size_t n = 0;
    for (size_t i = 0; i < s->rccap; i++)
        if (s->rc[i].used) recs[n++] = s->rc[i].rec;
    for (size_t i = 0; i < n; i++)
        if (!keep(recs[i], ctx)) semantic_index_remove(s, recs[i]);
    free(recs);
}
