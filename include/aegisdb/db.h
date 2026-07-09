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

    uint64_t started_ms;     /* server start time (epoch ms) for uptime stats */
    Metrics metrics;         /* operational counters (see stats op) */
    uint64_t next_id;        /* monotonic id allocator for persisted records */
    pthread_mutex_t id_lock; /* guards next_id */
    pthread_rwlock_t index_lock; /* guards the in-memory indexes (T051) */
    /* Guards the log-file lifecycle so a reader can resolve an id->offset under
     * index_lock, then drop it and do the disk read holding only this lock.
     * Only compaction's log swap takes it for write; appends never do (they
     * never invalidate an existing offset). Always acquire AFTER index_lock. */
    pthread_rwlock_t log_lock;

    char path_log[1200];
    char path_index[1200];
    char path_meta[1200];
    char path_sem[1200]; /* HNSW graph checkpoint */

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

/* Build the HNSW graph off-lock if the live vector count has crossed the ANN
 * threshold and no graph exists yet. Driven by the maintenance thread so the
 * expensive build never blocks readers/writers. Returns 1 if a graph was built,
 * 0 if nothing to do, -1 on failure (retried on a later tick). */
int db_semantic_build_step(AegisDB *db);

/* Result of a successful db_snapshot(): where it landed and what it covers. */
typedef struct {
    char dir[1300];        /* the snapshot directory that was written */
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