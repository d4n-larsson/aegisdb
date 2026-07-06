/* AegisDB lifecycle: open/close, time, id allocation, metadata (supports T019/T025). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aegisdb/db.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "aegisdb/fsutil.h"
#include "aegisdb/logging.h"
#include "aegisdb/recovery.h"

uint64_t db_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

uint64_t db_next_id(AegisDB *db) {
    pthread_mutex_lock(&db->id_lock);
    uint64_t id = db->next_id++;
    pthread_mutex_unlock(&db->id_lock);
    return id;
}

int db_save_metadata(AegisDB *db) {
    char tmp[1300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", db->path_meta);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    uint8_t buf[32] = {'A', 'M', 'E', 'T', 'A'};
    uint32_t ver = 1;
    memcpy(buf + 5, &ver, 4);
    pthread_mutex_lock(&db->id_lock);
    uint64_t nid = db->next_id;
    pthread_mutex_unlock(&db->id_lock);
    memcpy(buf + 9, &nid, 8);
    int ok = (fwrite(buf, 1, sizeof(buf), f) == sizeof(buf));
    fclose(f);
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    return rename(tmp, db->path_meta);
}

int db_checkpoint(AegisDB *db) {
    /* Capture a consistent (hash, covered offset, next_id) tuple under the read
     * lock, then write it. Readers proceed; writers pause only for the write. */
    pthread_rwlock_rdlock(&db->index_lock);
    pthread_mutex_lock(&db->id_lock);
    uint64_t nid = db->next_id;
    pthread_mutex_unlock(&db->id_lock);
    uint64_t covered = (uint64_t)db->log.size;
    int rv = hash_index_save(db->hash, db->path_index, covered, nid);
    /* Persist the HNSW graph at the same covered offset so recovery can load it
     * and replay only the tail. No-op (drops any stale file) below the ANN
     * threshold, where search is the exact scan. */
    int rv2 = semantic_index_save(db->sem, db->path_sem, covered);
    pthread_rwlock_unlock(&db->index_lock);
    return (rv == 0 && rv2 == 0) ? 0 : -1;
}

/* A snapshot name becomes a single path component under snapshots/, so it must
 * be non-empty and free of separators or dot-traversal. */
static int snapshot_name_ok(const char *name) {
    if (!name || !*name) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') return 0;
    return 1;
}

/* Copy the first `n` bytes of the open log fd to `dst_path`. pread leaves the fd
 * offset untouched, so concurrent appends (which pwrite past `n`) are safe. */
static int copy_log_prefix(int src_fd, const char *dst_path, uint64_t n) {
    FILE *dst = fopen(dst_path, "wb");
    if (!dst) return -1;
    char buf[65536];
    uint64_t done = 0;
    int ok = 1;
    while (done < n) {
        size_t want = n - done < sizeof(buf) ? (size_t)(n - done) : sizeof(buf);
        ssize_t got = pread(src_fd, buf, want, (off_t)done);
        if (got <= 0) { ok = 0; break; } /* short read: log shrank unexpectedly */
        if (fwrite(buf, 1, (size_t)got, dst) != (size_t)got) { ok = 0; break; }
        done += (uint64_t)got;
    }
    if (fflush(dst) != 0) ok = 0;
    if (fclose(dst) != 0) ok = 0;
    if (!ok) unlink(dst_path);
    return ok ? 0 : -1;
}

/* Write a standalone metadata.db carrying `nid` as the next_id floor (same
 * AMETA format as db_save_metadata, but to an arbitrary path). */
static int write_meta_file(const char *path, uint64_t nid) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint8_t buf[32] = {'A', 'M', 'E', 'T', 'A'};
    uint32_t ver = 1;
    memcpy(buf + 5, &ver, 4);
    memcpy(buf + 9, &nid, 8);
    int ok = (fwrite(buf, 1, sizeof(buf), f) == sizeof(buf));
    if (fclose(f) != 0) ok = 0;
    if (!ok) unlink(path);
    return ok ? 0 : -1;
}

int db_snapshot(AegisDB *db, const char *name, DbSnapshotInfo *out) {
    if (!snapshot_name_ok(name)) return DB_SNAPSHOT_BADNAME;

    char dir[1300];
    snprintf(dir, sizeof(dir), "%s/snapshots/%s", db->config.data_dir, name);
    if (fs_mkdir_p(dir) != 0) {
        LOG_ERROR("snapshot: cannot create %s", dir);
        return DB_SNAPSHOT_ERR;
    }

    /* Flush pending appends so the durable prefix we copy is as complete as
     * possible, then capture a consistent (offset, next_id, count) tuple. */
    log_fsync(&db->log);

    pthread_rwlock_rdlock(&db->index_lock);
    size_t live = 0;
    for (size_t i = 0; i < db->hash->cap; i++)
        if (db->hash->buckets[i].used && !db->hash->buckets[i].deleted) live++;
    pthread_mutex_lock(&db->id_lock);
    uint64_t nid = db->next_id;
    pthread_mutex_unlock(&db->id_lock);

    /* log_lock (read) blocks only compaction's log swap; appends continue and
     * land past `covered`, so [0, covered) is a stable, valid frame prefix. */
    pthread_rwlock_rdlock(&db->log_lock);
    uint64_t covered = (uint64_t)db->log.size;
    int src_fd = db->log.fd;
    char logpath[1400];
    snprintf(logpath, sizeof(logpath), "%s/memory.log", dir);
    int crc = copy_log_prefix(src_fd, logpath, covered);
    pthread_rwlock_unlock(&db->log_lock);
    pthread_rwlock_unlock(&db->index_lock);

    if (crc != 0) {
        LOG_ERROR("snapshot: log copy failed (%s)", logpath);
        return DB_SNAPSHOT_ERR;
    }

    char metapath[1400];
    snprintf(metapath, sizeof(metapath), "%s/metadata.db", dir);
    if (write_meta_file(metapath, nid) != 0) {
        LOG_ERROR("snapshot: metadata write failed (%s)", metapath);
        return DB_SNAPSHOT_ERR;
    }

    uint64_t created = db_now_ms();
    char manifest[1400], man[768];
    int mn = snprintf(man, sizeof(man),
                      "{\"format\":1,\"created_ms\":%llu,\"version\":\"%s\","
                      "\"log_size\":%llu,\"record_count\":%zu,\"next_id\":%llu,"
                      "\"embedding_dim\":%zu}\n",
                      (unsigned long long)created, AEGIS_VERSION_STRING,
                      (unsigned long long)covered, live,
                      (unsigned long long)nid, db->config.embedding_dimensions);
    snprintf(manifest, sizeof(manifest), "%s/manifest.json", dir);
    FILE *mf = fopen(manifest, "wb");
    if (!mf || mn < 0 || fwrite(man, 1, (size_t)mn, mf) != (size_t)mn) {
        if (mf) fclose(mf);
        unlink(manifest);
        LOG_ERROR("snapshot: manifest write failed (%s)", manifest);
        return DB_SNAPSHOT_ERR;
    }
    fclose(mf);

    if (out) {
        snprintf(out->dir, sizeof(out->dir), "%s", dir);
        out->log_size = covered;
        out->next_id = nid;
        out->created_ms = created;
        out->record_count = live;
    }
    LOG_INFO("snapshot: wrote %s (%llu log bytes, %zu records)", dir,
             (unsigned long long)covered, live);
    return DB_SNAPSHOT_OK;
}

/* Read the persisted next_id from metadata.db, or 0 if absent/unreadable. It is
 * a high-water mark used as a floor at recovery so next_id can't regress. */
static uint64_t load_metadata_next_id(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t buf[32];
    uint64_t nid = 0;
    if (fread(buf, 1, sizeof(buf), f) == sizeof(buf) &&
        memcmp(buf, "AMETA", 5) == 0)
        memcpy(&nid, buf + 9, 8);
    fclose(f);
    return nid;
}

int db_open(AegisDB *db, const Config *cfg) {
    memset(db, 0, sizeof(*db));
    db->config = *cfg;
    db->next_id = 1;

    if (fs_mkdir_p(cfg->data_dir) != 0) {
        LOG_ERROR("cannot create data dir: %s", cfg->data_dir);
        return -1;
    }
    LOG_DEBUG("opening database in %s (embedding-dim %zu, working-capacity %u, "
              "fsync-batch %zu)",
              cfg->data_dir, cfg->embedding_dimensions, cfg->working_capacity,
              cfg->fsync_batch_size);
    snprintf(db->path_log, sizeof(db->path_log), "%s/memory.log", cfg->data_dir);
    snprintf(db->path_index, sizeof(db->path_index), "%s/memory.index",
             cfg->data_dir);
    snprintf(db->path_meta, sizeof(db->path_meta), "%s/metadata.db",
             cfg->data_dir);
    snprintf(db->path_sem, sizeof(db->path_sem), "%s/memory.sem",
             cfg->data_dir);

    if (pthread_mutex_init(&db->id_lock, NULL) != 0) return -1;
    if (pthread_rwlock_init(&db->index_lock, NULL) != 0) {
        pthread_mutex_destroy(&db->id_lock);
        return -1;
    }
    if (pthread_rwlock_init(&db->log_lock, NULL) != 0) {
        pthread_rwlock_destroy(&db->index_lock);
        pthread_mutex_destroy(&db->id_lock);
        return -1;
    }

    if (log_open(&db->log, db->path_log, config_effective_fsync_batch(cfg)) !=
        0) {
        LOG_ERROR("cannot open log: %s", db->path_log);
        goto fail_locks;
    }
    db->hash = hash_index_create();
    db->time = time_index_create();
    db->tags = tag_index_create();
    db->sem = semantic_index_create(cfg->embedding_dimensions,
                                    cfg->ann_threshold, cfg->ann_ef_search,
                                    cfg->ann_quantize);
    db->working =
        working_store_create(cfg->working_capacity, cfg->default_ttl_ms);
    if (!db->hash || !db->time || !db->tags || !db->sem || !db->working) {
        LOG_ERROR("index allocation failed");
        goto fail_indexes;
    }

    long loaded = recovery_run(db);
    if (loaded < 0) {
        LOG_ERROR("recovery failed");
        goto fail_indexes;
    }
    LOG_INFO("recovery complete: %ld records loaded", loaded);

    /* metadata.db carries a high-water next_id from the last clean checkpoint.
     * Use it as a floor: a tail-truncating crash (or a stale/rejected index
     * checkpoint) can leave the log-derived next_id below an already-issued id,
     * which would let a future insert reuse it (#18). */
    uint64_t meta_next = load_metadata_next_id(db->path_meta);
    if (meta_next > db->next_id) {
        LOG_WARN("next_id from log scan (%llu) is below metadata high-water "
                 "(%llu); using the latter to avoid id reuse",
                 (unsigned long long)db->next_id, (unsigned long long)meta_next);
        db->next_id = meta_next;
    }

    db->started_ms = db_now_ms();
    db->running = 1;
    return 0;

fail_indexes:
    hash_index_free(db->hash);
    time_index_free(db->time);
    tag_index_free(db->tags);
    semantic_index_free(db->sem);
    working_store_free(db->working);
    log_close(&db->log);
fail_locks:
    pthread_mutex_destroy(&db->id_lock);
    pthread_rwlock_destroy(&db->index_lock);
    pthread_rwlock_destroy(&db->log_lock);
    return -1;
}

void db_close(AegisDB *db) {
    db->running = 0;
    LOG_DEBUG("closing database: flushing log and persisting index/metadata");
    log_fsync(&db->log);
    if (db_checkpoint(db) != 0)
        LOG_WARN("failed to persist checkpoint to %s", db->path_index);
    if (db_save_metadata(db) != 0)
        LOG_WARN("failed to persist metadata to %s", db->path_meta);
    hash_index_free(db->hash);
    time_index_free(db->time);
    tag_index_free(db->tags);
    semantic_index_free(db->sem);
    working_store_free(db->working);
    log_close(&db->log);
    pthread_mutex_destroy(&db->id_lock);
    pthread_rwlock_destroy(&db->index_lock);
    pthread_rwlock_destroy(&db->log_lock);
}