/* AegisDB aggregate: storage engine + indexes + runtime state. */
#ifndef AEGISDB_DB_H
#define AEGISDB_DB_H

#include <pthread.h>

#include "aegisdb/config.h"
#include "aegisdb/hash_index.h"
#include "aegisdb/log.h"
#include "aegisdb/semantic_index.h"
#include "aegisdb/tag_index.h"
#include "aegisdb/time_index.h"
#include "aegisdb/working_buffer.h"

typedef struct {
    Config config;
    LogFile log;

    HashIndex *hash;     /* id -> log location (Phase 1) */
    TimeIndex *time;     /* created -> ids (Phase 2) */
    TagIndex *tags;      /* tag -> ids (Phase 2) */
    SemanticIndex *sem;  /* embedding ANN (Phase 3) */
    WorkingStore *working; /* volatile sessions (Phase 4) */

    uint64_t started_ms;     /* server start time (epoch ms) for uptime stats */
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

#endif /* AEGISDB_DB_H */