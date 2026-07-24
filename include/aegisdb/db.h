/* AegisDB aggregate: storage engine + indexes + runtime state. */
#ifndef AEGISDB_DB_H
#define AEGISDB_DB_H

#include <pthread.h>
#include <stdatomic.h>

#include "aegisdb/config.h"
#include "aegisdb/hash_index.h"
#include "aegisdb/log.h"
#include "aegisdb/semantic_index.h"
#include "aegisdb/tag_index.h"
#include "aegisdb/tenant.h"
#include "aegisdb/time_index.h"
#include "aegisdb/working_buffer.h"

/* Per-operation index for the metrics counters. Order defines the JSON keys in
 * the stats `metrics.by_op` object (see query_engine.c). */
typedef enum {
    MOP_PING = 0, MOP_INSERT, MOP_GET, MOP_UPDATE, MOP_DELETE, MOP_SEARCH,
    MOP_COUNT, MOP_PROMOTE, MOP_RELATE, MOP_TRAVERSE, MOP_STATS,
    MOP_OTHER, /* unknown / missing operation */
    MOP__N
} MetricOp;

/* Monotonic operational counters, incremented per request from the io-threads;
 * lock-free atomics. Exposed via the stats op for external scraping. */
typedef struct {
    atomic_uint_fast64_t requests;        /* all dispatched requests */
    atomic_uint_fast64_t errors;          /* responses with ok:false */
    atomic_uint_fast64_t unauthorized;    /* auth rejections (subset of errors) */
    atomic_uint_fast64_t dispatch_micros; /* cumulative in-dispatch time (µs) */
    atomic_uint_fast64_t by_op[MOP__N];   /* per-operation request count */
    /* Memory-quality outcomes (ROADMAP 3.3 observability): cumulative records
     * removed by each maintenance policy, so operators can watch dedup/decay/
     * erase activity — not just request counts. */
    atomic_uint_fast64_t memories_merged;    /* records consolidate merged away */
    atomic_uint_fast64_t memories_forgotten; /* records forget aged out */
    atomic_uint_fast64_t memories_purged;    /* records purge erased (RTBF) */
} Metrics;

typedef struct {
    Config config;
    LogFile log;

    HashIndex *hash;     /* id -> log location (Phase 1) */
    TimeIndex *time;     /* created -> ids (Phase 2) */
    TagIndex *tags;      /* tag -> ids (Phase 2) */
    SemanticIndex *sem;  /* embedding ANN (Phase 3) */
    WorkingStore *working; /* volatile sessions (Phase 4) */
    TenantTable *tenants;  /* per-namespace usage + rate limiting (multi-tenant) */
    /* Replication handles (owned by main); NULL when not configured. Opaque here
     * to avoid an include cycle — see replication.h. */
    struct ReplicationSource *repl_source;     /* primary: serves the log stream */
    struct ReplicationFollower *repl_follower;  /* replica: follows a primary */

    uint64_t started_ms;     /* server start time (epoch ms) for uptime stats */
    Metrics metrics;         /* operational counters (see stats op) */
    /* Cached total in-RAM index bytes, sampled by the maintenance thread and
     * read lock-free on the write path to enforce --max-index-bytes. */
    atomic_uint_fast64_t index_bytes;
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

    char path_log[AEGIS_PATH_MAX];
    char path_index[AEGIS_PATH_MAX];
    char path_meta[AEGIS_PATH_MAX];
    char path_sem[AEGIS_PATH_MAX]; /* HNSW graph checkpoint */

    volatile int running;
} AegisDB;

/* Open the database: create data dir, open log, build indexes, run recovery. */
int db_open(AegisDB *db, const Config *cfg);
void db_close(AegisDB *db);

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
    char dir[AEGIS_PATH_MAX];        /* the snapshot directory that was written */
    uint64_t log_size;     /* durable log bytes captured (the covered offset) */
    uint64_t next_id;      /* id high-water at snapshot time (restore floor) */
    uint64_t created_ms;   /* wall-clock time the snapshot was taken */
    size_t record_count;   /* live (non-tombstone) records at snapshot time */
} DbSnapshotInfo;

#define DB_SNAPSHOT_OK       0
#define DB_SNAPSHOT_ERR     (-1) /* mkdir / copy / write failure */
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