/* Background compaction + TTL sweep (T033). */
#include "aegisdb/compaction.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/record.h"

typedef struct {
    uint64_t id;
    uint64_t offset;
    uint32_t length;
    uint8_t type;
} NewLoc;

int compaction_run_once(AegisDB *db) {
    char newpath[1300];
    snprintf(newpath, sizeof(newpath), "%s.compaction", db->path_log);
    unlink(newpath); /* discard any stale partial file */

    pthread_rwlock_wrlock(&db->index_lock);

    LogFile newlog;
    if (log_open(&newlog, newpath, db->config.fsync_batch_size) != 0) {
        pthread_rwlock_unlock(&db->index_lock);
        return -1;
    }

    HashIndex *h = db->hash;
    NewLoc *locs = malloc((h->count ? h->count : 1) * sizeof(NewLoc));
    if (!locs) {
        log_close(&newlog);
        unlink(newpath);
        pthread_rwlock_unlock(&db->index_lock);
        return -1;
    }
    size_t nloc = 0;
    int failed = 0;

    for (size_t i = 0; i < h->cap && !failed; i++) {
        const HashEntry *e = &h->buckets[i];
        if (!e->used || e->deleted) continue;
        uint8_t *buf = NULL;
        size_t len = 0;
        if (log_read(&db->log, e->offset, &buf, &len) != 0) continue;
        uint64_t off = 0;
        if (log_append(&newlog, buf, len, &off) != 0) {
            free(buf);
            failed = 1;
            break;
        }
        free(buf);
        locs[nloc].id = e->id;
        locs[nloc].offset = off;
        locs[nloc].length = (uint32_t)len;
        locs[nloc].type = e->type;
        nloc++;
    }

    if (failed) {
        free(locs);
        log_close(&newlog);
        unlink(newpath);
        pthread_rwlock_unlock(&db->index_lock);
        return -1;
    }

    log_fsync(&newlog);
    log_close(&newlog);
    log_close(&db->log);

    if (rename(newpath, db->path_log) != 0) {
        /* try to reopen the original log so the server keeps working */
        log_open(&db->log, db->path_log, db->config.fsync_batch_size);
        free(locs);
        pthread_rwlock_unlock(&db->index_lock);
        return -1;
    }
    if (log_open(&db->log, db->path_log, db->config.fsync_batch_size) != 0) {
        free(locs);
        pthread_rwlock_unlock(&db->index_lock);
        return -1;
    }

    /* Rebuild hash index with the new offsets (time/tag/sem indexes are
     * offset-independent and remain valid). */
    HashIndex *nh = hash_index_create();
    if (!nh) {
        free(locs);
        pthread_rwlock_unlock(&db->index_lock);
        return -1;
    }
    for (size_t i = 0; i < nloc; i++)
        hash_index_put(nh, locs[i].id, locs[i].offset, locs[i].length,
                       locs[i].type, 0);
    hash_index_free(db->hash);
    db->hash = nh;
    free(locs);

    pthread_rwlock_unlock(&db->index_lock);
    fprintf(stderr, "[aegisdb] compaction complete: %zu live records\n", nloc);
    return 0;
}

struct Compactor {
    AegisDB *db;
    pthread_t thread;
    unsigned sweep_sec;
    unsigned compact_sec;
    volatile int stop;
};

static void *maint_loop(void *arg) {
    Compactor *c = arg;
    unsigned elapsed = 0;
    while (!c->stop) {
        sleep(1);
        if (c->stop) break;
        elapsed++;
        if (c->sweep_sec && (elapsed % c->sweep_sec) == 0)
            working_store_sweep(c->db->working, db_now_ms());
        if (c->compact_sec && (elapsed % c->compact_sec) == 0)
            compaction_run_once(c->db);
    }
    return NULL;
}

Compactor *compaction_start(AegisDB *db, unsigned sweep_sec,
                            unsigned compact_sec) {
    Compactor *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->db = db;
    c->sweep_sec = sweep_sec;
    c->compact_sec = compact_sec;
    if (pthread_create(&c->thread, NULL, maint_loop, c) != 0) {
        free(c);
        return NULL;
    }
    return c;
}

void compaction_stop(Compactor *c) {
    if (!c) return;
    c->stop = 1;
    pthread_join(c->thread, NULL);
    free(c);
}