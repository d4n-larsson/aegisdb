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
#include "aegisdb/record.h"
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
    char tmp[AEGIS_PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", db->path_meta) >= (int)sizeof(tmp))
        return -1; /* path too long: refuse rather than write a truncated name */
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
    /* Order the data before the rename: without fsync the atomic rename can be
     * durable while the 32 bytes it points at are not, so a crash can leave a
     * present-but-empty metadata.db. load_metadata_next_id then finds no AMETA
     * magic and drops the next_id floor — reintroducing the id-reuse hazard
     * (#18) this file exists to prevent. */
    if (ok && (fflush(f) != 0 || fsync(fileno(f)) != 0)) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, db->path_meta) != 0) return -1;
    /* fsync the directory so the rename itself survives a crash. */
    return fs_fsync_dir(db->config.data_dir);
}

uint64_t db_index_bytes(AegisDB *db) {
    pthread_rwlock_rdlock(&db->index_lock);
    uint64_t total = (uint64_t)hash_index_bytes(db->hash) +
                     time_index_bytes(db->time) + tag_index_bytes(db->tags) +
                     semantic_index_bytes(db->sem);
    pthread_rwlock_unlock(&db->index_lock);
    return total;
}

int db_checkpoint(AegisDB *db) {
    /* Checkpoints are encrypted with the same key as the log when encryption is
     * enabled, so recovery can trust and load them (and full-scan only the
     * tail) instead of rebuilding from the whole encrypted log. */
    const uint8_t *key =
        db->config.encryption_enabled ? db->config.encryption_key : NULL;
    /* Capture a consistent (hash, covered offset, next_id) tuple under the read
     * lock, then write it. Readers proceed; writers pause only for the write. */
    pthread_rwlock_rdlock(&db->index_lock);
    pthread_mutex_lock(&db->id_lock);
    uint64_t nid = db->next_id;
    pthread_mutex_unlock(&db->id_lock);
    uint64_t covered = (uint64_t)db->log.size;
    int rv = hash_index_save(db->hash, db->path_index, covered, nid, key);
    /* Persist the HNSW graph at the same covered offset so recovery can load it
     * and replay only the tail. No-op (drops any stale file) below the ANN
     * threshold, where search is the exact scan. */
    int rv2 = semantic_index_save(db->sem, db->path_sem, covered, key);
    pthread_rwlock_unlock(&db->index_lock);
    return (rv == 0 && rv2 == 0) ? 0 : -1;
}

/* Deferred, off-lock HNSW build (driven by the maintenance thread).
 *
 * Crossing the ANN threshold used to build the graph inline under the index
 * write lock, freezing every reader and writer for the whole build (tens of
 * seconds at scale). This runs the build the way compaction runs a log rewrite:
 * snapshot under a brief write lock, build with no lock held, then install under
 * a brief write lock. Writes that race the build keep updating the dense array
 * (which still answers searches) and are journalled as deltas; a catch-up loop
 * replays those deltas into the new graph off-lock until the residual is small
 * enough to fold into the install under the lock.
 *
 * Returns 1 if a graph was built and installed, 0 if there was nothing to do,
 * -1 on failure (the dense array remains authoritative and it retries later). */
#define SEM_BUILD_COMMIT_MAX 512 /* deltas small enough to replay under the lock */
#define SEM_BUILD_MAX_ROUNDS 16  /* off-lock catch-up rounds before giving up */

static void build_abort_locked(AegisDB *db, SemBuildJob *job) {
    pthread_rwlock_wrlock(&db->index_lock);
    semantic_index_build_abort(db->sem, job);
    pthread_rwlock_unlock(&db->index_lock);
}

int db_semantic_build_step(AegisDB *db) {
    if (!db->sem) return 0;

    /* Phase 1: cheap poll under the read lock. */
    pthread_rwlock_rdlock(&db->index_lock);
    int pending = semantic_index_needs_build(db->sem);
    pthread_rwlock_unlock(&db->index_lock);
    if (!pending) return 0;

    /* Phase 2: snapshot the dense vectors, enter building mode. */
    pthread_rwlock_wrlock(&db->index_lock);
    SemBuildJob *job = semantic_index_build_begin(db->sem);
    size_t snap_n = semantic_index_count(db->sem);
    pthread_rwlock_unlock(&db->index_lock);
    if (!job) return 0; /* raced away / OOM: retry next tick */

    LOG_INFO("semantic index: building HNSW graph from %zu vectors off-lock",
             snap_n);

    /* Phase 3: build the graph with no lock held — the expensive part. */
    if (semantic_index_build_run(job) != 0) {
        build_abort_locked(db, job);
        LOG_WARN("semantic index: off-lock build failed; keeping exact scan");
        return -1;
    }

    /* Phase 4: catch the graph up to writes that raced the build. Take the
     * journalled deltas under the lock (brief) and replay them off-lock; repeat
     * until the residual shrinks to a batch small enough to fold into the
     * install under the lock. If writes keep arriving faster than the replay can
     * drain them, the residual never shrinks — rather than block writers with a
     * huge under-lock replay (the very stall this change removes), abort and
     * keep serving from the exact dense scan; a later tick retries. */
    int converged = 0;
    for (int round = 0; round < SEM_BUILD_MAX_ROUNDS; round++) {
        pthread_rwlock_wrlock(&db->index_lock);
        int failed = semantic_index_build_failed(db->sem);
        size_t took = failed ? 0 : semantic_index_build_take_deltas(db->sem, job);
        pthread_rwlock_unlock(&db->index_lock);

        if (failed) {
            build_abort_locked(db, job);
            LOG_WARN("semantic index: build journal OOM; keeping exact scan");
            return -1;
        }
        if (semantic_index_build_apply(job) != 0) {
            build_abort_locked(db, job);
            return -1;
        }
        if (took <= SEM_BUILD_COMMIT_MAX) {
            converged = 1;
            break; /* residual is small; commit under the lock below */
        }
    }
    if (!converged) {
        build_abort_locked(db, job);
        LOG_WARN("semantic index: writes outpaced the off-lock build; keeping "
                 "exact scan (will retry)");
        return -1;
    }

    /* Phase 5: install under the write lock, replaying the small residual. */
    pthread_rwlock_wrlock(&db->index_lock);
    int rc = semantic_index_build_commit(db->sem, job);
    size_t live = semantic_index_count(db->sem);
    pthread_rwlock_unlock(&db->index_lock);
    if (rc != 0) {
        LOG_WARN("semantic index: build commit failed; keeping exact scan");
        return -1;
    }
    LOG_INFO("semantic index: HNSW graph installed (%zu vectors)", live);
    return 1;
}

int db_replica_apply(AegisDB *db, uint64_t offset, const uint8_t *payload,
                     size_t len) {
    MemoryRecord r;
    if (record_decode(payload, len, &r) != 0) return -1;

    /* Diff against the record's prior live version so update/delete drop stale
     * secondary-index entries before the new version is applied. */
    const HashEntry *prior = hash_index_get(db->hash, r.id);
    if (prior && !prior->deleted) {
        uint8_t *pbuf = NULL;
        size_t plen = 0;
        if (log_read(&db->log, prior->offset, &pbuf, &plen) == 0) {
            MemoryRecord p;
            if (record_decode(pbuf, plen, &p) == 0) {
                time_index_remove(db->time, p.created, p.id);
                for (size_t i = 0; i < p.tag_count; i++)
                    tag_index_remove(db->tags, p.tags[i], p.id);
                if (p.embedding_dim && p.vec_count)
                    semantic_index_remove(db->sem, p.id);
                record_free(&p);
            }
            free(pbuf);
        }
    }

    if (hash_index_put(db->hash, r.id, offset, (uint32_t)len, (uint8_t)r.type,
                       (uint8_t)(r.deleted ? 1 : 0), r.expires_at) != 0) {
        record_free(&r);
        return -1;
    }
    if (!r.deleted) {
        time_index_add(db->time, r.created, r.id);
        for (size_t i = 0; i < r.tag_count; i++)
            tag_index_add(db->tags, r.tags[i], r.id);
        if (r.embedding_dim == db->config.embedding_dimensions && r.embedding &&
            r.vec_count)
            semantic_index_add(db->sem, r.id, r.embedding, r.vec_count,
                               r.embedding_dim);
    }

    pthread_mutex_lock(&db->id_lock);
    if (r.id + 1 > db->next_id) db->next_id = r.id + 1;
    pthread_mutex_unlock(&db->id_lock);
    record_free(&r);
    return 0;
}

int db_reset_replica(AegisDB *db) {
    if (log_truncate(&db->log, 0) != 0) return -1;
    HashIndex *nh = hash_index_create();
    TimeIndex *nt = time_index_create();
    TagIndex *ntag = tag_index_create();
    if (!nh || !nt || !ntag) {
        hash_index_free(nh);
        time_index_free(nt);
        tag_index_free(ntag);
        return -1;
    }
    hash_index_free(db->hash);
    db->hash = nh;
    time_index_free(db->time);
    db->time = nt;
    tag_index_free(db->tags);
    db->tags = ntag;
    semantic_index_clear(db->sem);
    pthread_mutex_lock(&db->id_lock);
    db->next_id = 1;
    pthread_mutex_unlock(&db->id_lock);
    return 0;
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
    char buf[AEGIS_IO_BUF_SIZE];
    uint64_t done = 0;
    int ok = 1;
    while (done < n) {
        size_t want = n - done < sizeof(buf) ? (size_t)(n - done) : sizeof(buf);
        ssize_t got = pread(src_fd, buf, want, (off_t)done);
        if (got <= 0) { ok = 0; break; } /* short read: log shrank unexpectedly */
        if (fwrite(buf, 1, (size_t)got, dst) != (size_t)got) { ok = 0; break; }
        done += (uint64_t)got;
    }
    if (ok && (fflush(dst) != 0 || fsync(fileno(dst)) != 0)) ok = 0;
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
    if (ok && (fflush(f) != 0 || fsync(fileno(f)) != 0)) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) unlink(path);
    return ok ? 0 : -1;
}

int db_snapshot(AegisDB *db, const char *name, DbSnapshotInfo *out) {
    if (!snapshot_name_ok(name)) return DB_SNAPSHOT_BADNAME;

    char dir[AEGIS_PATH_MAX];
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
    char logpath[AEGIS_PATH_MAX];
    if (snprintf(logpath, sizeof(logpath), "%s/memory.log", dir) >= (int)sizeof(logpath)) {
        pthread_rwlock_unlock(&db->log_lock);
        pthread_rwlock_unlock(&db->index_lock);
        LOG_ERROR("snapshot: directory path too long");
        return DB_SNAPSHOT_ERR;
    }
    int crc = copy_log_prefix(src_fd, logpath, covered);
    pthread_rwlock_unlock(&db->log_lock);
    pthread_rwlock_unlock(&db->index_lock);

    if (crc != 0) {
        LOG_ERROR("snapshot: log copy failed (%s)", logpath);
        return DB_SNAPSHOT_ERR;
    }

    char metapath[AEGIS_PATH_MAX];
    if (snprintf(metapath, sizeof(metapath), "%s/metadata.db", dir) >= (int)sizeof(metapath)) {
        LOG_ERROR("snapshot: directory path too long");
        return DB_SNAPSHOT_ERR;
    }
    if (write_meta_file(metapath, nid) != 0) {
        LOG_ERROR("snapshot: metadata write failed (%s)", metapath);
        return DB_SNAPSHOT_ERR;
    }

    uint64_t created = db_now_ms();
    /* An encrypted snapshot is a byte copy of the encrypted log, so it stays
     * encrypted; record the key fingerprint (not the key) so restore can check
     * the operator supplied the right one. */
    char enc_fields[80] = "";
    if (db->config.encryption_enabled) {
        char fp[13];
        config_key_fingerprint(db->config.encryption_key, fp);
        snprintf(enc_fields, sizeof(enc_fields),
                 ",\"encrypted\":true,\"key_fingerprint\":\"%s\"", fp);
    }
    char manifest[AEGIS_PATH_MAX];
    char man[768]; /* manifest JSON content (not a path) */
    int mn = snprintf(man, sizeof(man),
                      "{\"format\":1,\"created_ms\":%llu,\"version\":\"%s\","
                      "\"log_size\":%llu,\"record_count\":%zu,\"next_id\":%llu,"
                      "\"embedding_dim\":%zu%s}\n",
                      (unsigned long long)created, AEGIS_VERSION_STRING,
                      (unsigned long long)covered, live,
                      (unsigned long long)nid, db->config.embedding_dimensions,
                      enc_fields);
    if (mn < 0 || (size_t)mn >= sizeof(man)) {
        LOG_ERROR("snapshot: manifest too large");
        return DB_SNAPSHOT_ERR; /* truncated: mn would over-read man in fwrite */
    }
    if (snprintf(manifest, sizeof(manifest), "%s/manifest.json", dir) >= (int)sizeof(manifest)) {
        LOG_ERROR("snapshot: directory path too long");
        return DB_SNAPSHOT_ERR;
    }
    FILE *mf = fopen(manifest, "wb");
    int mok = (mf && fwrite(man, 1, (size_t)mn, mf) == (size_t)mn);
    if (mok && (fflush(mf) != 0 || fsync(fileno(mf)) != 0)) mok = 0;
    if (mf && fclose(mf) != 0) mok = 0;
    if (!mok) {
        unlink(manifest);
        LOG_ERROR("snapshot: manifest write failed (%s)", manifest);
        return DB_SNAPSHOT_ERR;
    }
    /* Make the snapshot's directory entries durable so a crash right after this
     * call can't leave a snapshot reported as written but partially absent. */
    fs_fsync_dir(dir);

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

static void free_token_array(AuthToken *toks, size_t n) {
    if (!toks) return;
    for (size_t i = 0; i < n; i++) {
        free(toks[i].token);
        free(toks[i].namespace);
    }
    free(toks);
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
    if (pthread_rwlock_init(&db->auth_lock, NULL) != 0) {
        pthread_rwlock_destroy(&db->log_lock);
        pthread_rwlock_destroy(&db->index_lock);
        pthread_mutex_destroy(&db->id_lock);
        return -1;
    }
    /* Own a private deep copy of the token set: runtime token-admin mutates
     * db->config.auth_tokens, while the startup Config keeps (and frees) its own. */
    if (cfg->auth_token_count > 0) {
        AuthToken *copy = calloc(cfg->auth_token_count, sizeof(AuthToken));
        if (!copy) goto fail_locks;
        size_t i = 0;
        for (; i < cfg->auth_token_count; i++) {
            copy[i] = cfg->auth_tokens[i]; /* hash[], hashed, scope; dup ptrs below */
            copy[i].token = NULL;
            copy[i].namespace = NULL;
            if (cfg->auth_tokens[i].token &&
                !(copy[i].token = strdup(cfg->auth_tokens[i].token)))
                break;
            if (cfg->auth_tokens[i].namespace &&
                !(copy[i].namespace = strdup(cfg->auth_tokens[i].namespace)))
                break;
        }
        if (i != cfg->auth_token_count) { /* strdup OOM mid-copy */
            free_token_array(copy, i + 1);
            goto fail_locks;
        }
        db->config.auth_tokens = copy;
    } else {
        db->config.auth_tokens = NULL;
    }

    const uint8_t *log_key = cfg->encryption_enabled ? cfg->encryption_key : NULL;
    LogOpenStatus log_st;
    if (log_open(&db->log, db->path_log, config_effective_fsync_batch(cfg),
                 log_key, &log_st) != 0) {
        /* log_open already logged the specific reason (wrong key, mode
         * mismatch, I/O). */
        LOG_ERROR("cannot open log: %s", db->path_log);
        goto fail_locks;
    }
    db->hash = hash_index_create();
    db->time = time_index_create();
    db->tags = tag_index_create();
    db->sem = semantic_index_create(cfg->embedding_dimensions,
                                    cfg->ann_threshold, cfg->ann_ef_search,
                                    cfg->ann_quantize, cfg->ann_shard_target);
    db->working =
        working_store_create(cfg->working_capacity, cfg->default_ttl_ms);
    db->tenants = tenant_table_create();
    if (!db->hash || !db->time || !db->tags || !db->sem || !db->working ||
        !db->tenants) {
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
    tenant_table_free(db->tenants);
    log_close(&db->log);
fail_locks:
    pthread_mutex_destroy(&db->id_lock);
    pthread_rwlock_destroy(&db->index_lock);
    pthread_rwlock_destroy(&db->log_lock);
    pthread_rwlock_destroy(&db->auth_lock);
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
    tenant_table_free(db->tenants);
    free_token_array(db->config.auth_tokens, db->config.auth_token_count);
    log_close(&db->log);
    pthread_mutex_destroy(&db->id_lock);
    pthread_rwlock_destroy(&db->index_lock);
    pthread_rwlock_destroy(&db->log_lock);
    pthread_rwlock_destroy(&db->auth_lock);
}