/* AegisDB aggregate: storage engine + indexes + runtime state. */
#ifndef AEGISDB_DB_H
#define AEGISDB_DB_H

#include <pthread.h>
#include <stdatomic.h>

#include "aegisdb/config.h"
#include "aegisdb/edge_index.h"
#include "aegisdb/fact_index.h"
#include "aegisdb/hash_index.h"
#include "aegisdb/lexical_index.h"
#include "aegisdb/log.h"
#include "aegisdb/predicate_registry.h"
#include "aegisdb/semantic_index.h"
#include "aegisdb/tag_index.h"
#include "aegisdb/tenant.h"
#include "aegisdb/time_index.h"
#include "aegisdb/usage_index.h"
#include "aegisdb/working_buffer.h"

/* Per-operation index for the metrics counters. Order defines the JSON keys in
 * the stats `metrics.by_op` object (see query_engine.c). */
typedef enum {
    MOP_PING = 0,
    MOP_INSERT,
    MOP_GET,
    MOP_UPDATE,
    MOP_DELETE,
    MOP_SEARCH,
    MOP_COUNT,
    MOP_PROMOTE,
    MOP_RELATE,
    MOP_TRAVERSE,
    MOP_STATS,
    MOP_HISTORY,
    MOP_EXPORT,
    MOP_PURGE,
    MOP_CONSOLIDATE,
    MOP_FORGET,
    MOP_OTHER, /* unknown / missing operation, incl. admin (token_*, snapshot) */
    MOP__N
} MetricOp;

/* Upper bounds (microseconds) of the recall-latency histogram's finite buckets,
 * plus an implicit overflow bucket for anything slower (ROADMAP 3.3).
 *
 * Recall sits in an agent's inner loop, so the interesting range is sub-
 * millisecond to tens of milliseconds — a cumulative mean (dispatch_micros)
 * hides exactly the tail an operator needs to alert on. The bounds are dense
 * where recall should live and sparse past the point where it is already too
 * slow to matter. */
#define RECALL_HIST_N 12 /* 11 finite buckets + 1 overflow */
extern const uint64_t recall_hist_bounds[RECALL_HIST_N - 1];

/* Monotonic operational counters, incremented per request from the io-threads;
 * lock-free atomics. Exposed via the stats op for external scraping. */
typedef struct {
    atomic_uint_fast64_t requests;     /* all dispatched requests */
    atomic_uint_fast64_t errors;       /* responses with ok:false */
    atomic_uint_fast64_t unauthorized; /* auth rejections (subset of errors) */
    atomic_uint_fast64_t dispatch_micros; /* cumulative in-dispatch time (µs) */
    atomic_uint_fast64_t by_op[MOP__N];   /* per-operation request count */
    /* Memory-quality outcomes (ROADMAP 3.3 observability): cumulative records
     * removed by each maintenance policy, so operators can watch dedup/decay/
     * erase activity — not just request counts. */
    atomic_uint_fast64_t memories_merged; /* records consolidate merged away */
    atomic_uint_fast64_t memories_forgotten; /* records forget aged out */
    atomic_uint_fast64_t memories_purged;    /* records purge erased (RTBF) */
    /* Recall-latency histogram (ROADMAP 3.3): the distribution of `search`
     * dispatch time, so p95/p99 are observable rather than averaged away. Counts
     * are per-bucket (not cumulative); the stats op accumulates them into
     * Prometheus `le` semantics on the way out. */
    atomic_uint_fast64_t recall_hist[RECALL_HIST_N];
    atomic_uint_fast64_t recall_micros; /* summed latency, for the mean/_sum */
    atomic_uint_fast64_t
        recall_count; /* observations (== sum of the buckets) */
} Metrics;

typedef struct {
    Config config;
    LogFile log;

    HashIndex *hash;   /* id -> log location (Phase 1) */
    TimeIndex *time;   /* created -> ids (Phase 2) */
    TagIndex *tags;    /* tag -> ids (Phase 2) */
    LexicalIndex *lex; /* term -> postings, BM25 (ROADMAP 4.1); NULL when
                            * --no-lexical-index disabled it */
    FactIndex *facts;  /* subject/object/predicate -> records (ROADMAP 5.2);
                            * NULL when --no-fact-index disabled it */
    /* Declared fact vocabulary; NULL when --predicate-registry is unset, which
     * accepts any predicate. Loaded once at startup and immutable, so readers
     * need no lock. */
    PredicateRegistry *predicates;
    EdgeIndex *edges;      /* to_id -> incoming sources (ROADMAP 5.1); NULL when
                            * --no-edge-index disabled it. Only the *reverse*
                            * direction: a record is its own forward adjacency
                            * list, so a forward walk needs no index. */
    SemanticIndex *sem;    /* embedding ANN (Phase 3) */
    UsageIndex *usage;     /* id -> recall count/recency; NULL when
                            * --no-usage-feedback disabled it */
    WorkingStore *working; /* volatile sessions (Phase 4) */
    TenantTable
        *tenants; /* per-namespace usage + rate limiting (multi-tenant) */
    /* Replication handles (owned by main); NULL when not configured. Opaque here
     * to avoid an include cycle — see replication.h. */
    struct ReplicationSource *repl_source; /* primary: serves the log stream */
    struct ReplicationFollower *repl_follower; /* replica: follows a primary */

    uint64_t started_ms; /* server start time (epoch ms) for uptime stats */
    Metrics metrics;     /* operational counters (see stats op) */
    /* Cached total in-RAM index bytes, sampled by the maintenance thread and
     * read lock-free on the write path to enforce --max-index-bytes. */
    atomic_uint_fast64_t index_bytes;
    /* Inference job state (ROADMAP 5.3). `infer_cursor` is where the next
     * budgeted pass starts its scan; it only advances when a cap truncated the
     * previous one, so a pass that reached the end does not skip anything on
     * the next tick. The rest is what `stats` reports. */
    atomic_uint_fast64_t infer_cursor;
    /* Which namespace group the next pass starts with. The write budget is
     * shared across tenants, so visiting them in a fixed order would starve
     * whoever sorts last behind whoever is busiest. */
    atomic_uint_fast64_t infer_ns_cursor;
    atomic_uint_fast64_t derived_total; /* conclusions written since start */
    atomic_uint_fast64_t infer_last_ms;
    atomic_uint_fast64_t infer_deferred; /* a cap stopped the last pass */
    uint64_t next_id;        /* monotonic id allocator for persisted records */
    pthread_mutex_t id_lock; /* guards next_id */
    /* Bumped whenever compaction rewrites the log (offsets change). Replicas
     * detect the change and re-bootstrap, since their byte-offset cursor into
     * the old log is no longer valid. See docs/read-replica-design.md. */
    atomic_uint_fast64_t log_generation;
    pthread_rwlock_t index_lock; /* guards the in-memory indexes (T051) */
    /* Guards the log-file lifecycle so a reader can resolve an id->offset under
     * index_lock, then drop it and do the disk read holding only this lock.
     * Only compaction's log swap takes it for write; appends never do (they
     * never invalidate an existing offset). Always acquire AFTER index_lock. */
    pthread_rwlock_t log_lock;
    /* Guards config.auth_tokens for runtime token administration: readers
     * (auth resolution on every request) take it for read, token_add/revoke
     * take it for write. The DB owns its own deep copy of the token set (the
     * startup Config keeps its own), so runtime mutation is isolated. */
    pthread_rwlock_t auth_lock;
    /* Serializes compaction_run_once: both the maintenance thread and an inline
     * purge-driven compaction (handle_purge) can call it, and two at once would
     * clobber the shared scratch log and double-swap db->log. A second caller
     * skips (trylock) rather than waits. Independent of the other locks. */
    pthread_mutex_t compaction_lock;

    char path_log[AEGIS_PATH_MAX];
    char path_index[AEGIS_PATH_MAX];
    char path_meta[AEGIS_PATH_MAX];
    char path_sem[AEGIS_PATH_MAX];   /* HNSW graph checkpoint */
    char path_usage[AEGIS_PATH_MAX]; /* usage-feedback checkpoint */

    volatile int running;
} AegisDB;

/* Open the database: create data dir, open log, build indexes, run recovery. */
int db_open(AegisDB *db, const Config *cfg);
void db_close(AegisDB *db);

/* Index (add != 0) or unindex a record's fact in the fact indexes. A no-op when
 * the record carries none or --no-fact-index disabled them. Shared by the insert,
 * delete, replica-apply and recovery paths so they cannot drift. */
void db_fact_index_apply(AegisDB *db, const MemoryRecord *r, int add);

/* Current wall-clock time in epoch milliseconds. */
uint64_t db_now_ms(void);

/* Allocate the next persisted record id (thread-safe). */
uint64_t db_next_id(AegisDB *db);

/* Persist server metadata checkpoint (next_id, schema version). */
int db_save_metadata(AegisDB *db);

/* Persist a hash-index checkpoint (id -> log location, covered log size, and
 * next_id) so recovery can skip the covered prefix and replay only the tail.
 * Thread-safe. Returns 0/-1. */
int db_checkpoint(AegisDB *db);

/* Sum the approximate resident bytes of all in-RAM indexes (hash + time + tag +
 * semantic). Takes the index read lock and walks the indexes, so it is O(index
 * size) — call it off the hot path (the maintenance thread samples it into
 * db->index_bytes for the write path to read lock-free). */
uint64_t db_index_bytes(AegisDB *db);

/* Build the HNSW graph off-lock if the live vector count has crossed the ANN
 * threshold and no graph exists yet. Driven by the maintenance thread so the
 * expensive build never blocks readers/writers. Returns 1 if a graph was built,
 * 0 if nothing to do, -1 on failure (retried on a later tick). */
int db_semantic_build_step(AegisDB *db);

/* Run one inference pass (ROADMAP 5.3): read the live fact set, draw what the
 * registry's declarations imply, and write the conclusions as records with
 * their provenance. Driven by the maintenance thread. Returns the number of
 * conclusions written.
 *
 * A no-op unless --inference is set and a registry is loaded, and always a
 * no-op on a read-only replica — a follower that derived locally would append
 * frames its primary never sent. See docs/inference-design.md §5. */
size_t db_inference_step(AegisDB *db);

/* Apply one replicated log frame on a read-only replica: append the payload to
 * the local log (producing a byte-identical frame at `offset`) is done by the
 * caller; this updates the in-memory indexes to reflect the record, diffing
 * against its prior version so insert/update/delete all converge (mirrors what
 * the primary's write path does to the indexes). Caller holds the index write
 * lock. `offset`/`len` describe the just-appended frame. Returns 0/-1. */
int db_replica_apply(AegisDB *db, uint64_t offset, const uint8_t *payload,
                     size_t len);

/* Wipe a replica back to empty (truncate the local log, recreate empty
 * indexes), for re-bootstrapping after the primary compacted (its offsets
 * changed). Caller holds the index write lock. Returns 0/-1. */
int db_reset_replica(AegisDB *db);

/* Result of a successful db_snapshot(): where it landed and what it covers. */
typedef struct {
    char dir[AEGIS_PATH_MAX]; /* the snapshot directory that was written */
    uint64_t log_size;   /* durable log bytes captured (the covered offset) */
    uint64_t next_id;    /* id high-water at snapshot time (restore floor) */
    uint64_t created_ms; /* wall-clock time the snapshot was taken */
    size_t record_count; /* live (non-tombstone) records at snapshot time */
} DbSnapshotInfo;

#define DB_SNAPSHOT_OK 0
#define DB_SNAPSHOT_ERR (-1)     /* mkdir / copy / write failure */
#define DB_SNAPSHOT_BADNAME (-2) /* name empty or contains a path separator */

/* Write a consistent online snapshot under <data_dir>/snapshots/<name>/. The log
 * is append-only, so a snapshot is the durable log prefix [0, log_size) plus a
 * fresh metadata.db (the next_id floor) and a manifest.json; derived checkpoints
 * are omitted (recovery rebuilds them). Captured under log_lock so an in-flight
 * compaction cannot swap the log mid-copy; concurrent appends land past the
 * captured offset and are simply not included. Fills *out on success. Returns
 * DB_SNAPSHOT_OK, or a DB_SNAPSHOT_* error. Thread-safe. */
int db_snapshot(AegisDB *db, const char *name, DbSnapshotInfo *out);

#endif /* AEGISDB_DB_H */