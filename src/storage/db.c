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

static int mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
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

int db_open(AegisDB *db, const Config *cfg) {
    memset(db, 0, sizeof(*db));
    db->config = *cfg;
    db->next_id = 1;

    if (mkdir_p(cfg->data_dir) != 0) {
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

    if (pthread_mutex_init(&db->id_lock, NULL) != 0) return -1;
    if (pthread_rwlock_init(&db->index_lock, NULL) != 0) {
        pthread_mutex_destroy(&db->id_lock);
        return -1;
    }

    if (log_open(&db->log, db->path_log, cfg->fsync_batch_size) != 0) {
        LOG_ERROR("cannot open log: %s", db->path_log);
        goto fail_locks;
    }
    db->hash = hash_index_create();
    db->time = time_index_create();
    db->tags = tag_index_create();
    db->sem = semantic_index_create(cfg->embedding_dimensions);
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
    return -1;
}

void db_close(AegisDB *db) {
    db->running = 0;
    LOG_DEBUG("closing database: flushing log and persisting index/metadata");
    log_fsync(&db->log);
    if (hash_index_save(db->hash, db->path_index) != 0)
        LOG_WARN("failed to persist hash index to %s", db->path_index);
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
}