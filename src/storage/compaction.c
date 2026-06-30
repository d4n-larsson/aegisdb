/* Background compaction + TTL sweep (T033). */
#include "aegisdb/compaction.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/logging.h"
#include "aegisdb/record.h"

typedef struct {
    uint64_t id;
    uint64_t offset;
    uint32_t length;
    uint8_t type;
    uint8_t deleted;
} NewLoc;

/* Append one location entry to a growing array. Returns 0/-1. */
static int locs_push(NewLoc **locs, size_t *n, size_t *cap, NewLoc v) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        NewLoc *g = realloc(*locs, nc * sizeof(NewLoc));
        if (!g) return -1;
        *locs = g;
        *cap = nc;
    }
    (*locs)[(*n)++] = v;
    return 0;
}

/* Compact the log by rewriting only live records into a fresh log, then
 * swapping it in. The expensive copy runs WITHOUT the index lock: the source
 * region [0, snap_end) is immutable because the log is append-only, so readers
 * and writers proceed concurrently. Only the final drain-of-the-tail, log swap,
 * and hash rebuild hold the write lock, and the tail is bounded by the writes
 * that happened to race compaction rather than the whole live set. */
int compaction_run_once(AegisDB *db) {
    LOG_DEBUG("compaction: starting log rewrite");
    char newpath[1300];
    snprintf(newpath, sizeof(newpath), "%s.compaction", db->path_log);
    unlink(newpath); /* discard any stale partial file */

    /* Phase 1 (brief read lock): snapshot the live entries and the byte offset
     * up to which they were captured. */
    pthread_rwlock_rdlock(&db->index_lock);
    HashIndex *h = db->hash;
    NewLoc *snap = malloc((h->count ? h->count : 1) * sizeof(NewLoc));
    if (!snap) {
        pthread_rwlock_unlock(&db->index_lock);
        return -1;
    }
    size_t snap_n = 0;
    for (size_t i = 0; i < h->cap; i++) {
        const HashEntry *e = &h->buckets[i];
        if (!e->used || e->deleted) continue;
        snap[snap_n++] = (NewLoc){e->id, e->offset, e->length, e->type, 0};
    }
    uint64_t snap_end = (uint64_t)db->log.size;
    pthread_rwlock_unlock(&db->index_lock);

    /* Phase 2 (no lock): copy the snapshot's live frames into the scratch log. */
    LogFile newlog;
    if (log_open(&newlog, newpath, config_effective_fsync_batch(&db->config)) !=
        0) {
        LOG_ERROR("compaction: cannot open scratch log %s", newpath);
        free(snap);
        return -1;
    }
    NewLoc *locs = NULL;
    size_t nloc = 0, locs_cap = 0;
    int failed = 0;
    for (size_t i = 0; i < snap_n && !failed; i++) {
        uint8_t *buf = NULL;
        size_t len = 0;
        if (log_read(&db->log, snap[i].offset, &buf, &len) != 0) continue;
        uint64_t off = 0;
        if (log_append(&newlog, buf, len, &off) != 0) {
            free(buf);
            failed = 1;
            break;
        }
        free(buf);
        if (locs_push(&locs, &nloc, &locs_cap,
                      (NewLoc){snap[i].id, off, (uint32_t)len, snap[i].type, 0}))
            failed = 1;
    }
    free(snap);
    if (failed) {
        free(locs);
        log_close(&newlog);
        unlink(newpath);
        return -1;
    }

    /* Phase 3 (write lock): drain frames appended during phase 2, swap the log
     * in, and rebuild the hash. The tail is bounded by concurrent writes. */
    pthread_rwlock_wrlock(&db->index_lock);
    for (uint64_t tail = snap_end; tail < (uint64_t)db->log.size && !failed;) {
        uint8_t *buf = NULL;
        size_t len = 0;
        /* Any anomaly in the tail means we cannot faithfully reproduce it;
         * abort (keep the original log) rather than break-and-commit, which
         * would silently drop every live record after this point. */
        if (log_read(&db->log, tail, &buf, &len) != 0) {
            failed = 1;
            break;
        }
        MemoryRecord r;
        if (record_decode(buf, len, &r) != 0) {
            free(buf);
            failed = 1;
            break;
        }
        uint64_t off = 0;
        if (log_append(&newlog, buf, len, &off) != 0) {
            record_free(&r);
            free(buf);
            failed = 1;
            break;
        }
        if (locs_push(&locs, &nloc, &locs_cap,
                      (NewLoc){r.id, off, (uint32_t)len, (uint8_t)r.type,
                               (uint8_t)(r.deleted ? 1 : 0)}))
            failed = 1;
        record_free(&r);
        free(buf);
        tail += LOG_FRAME_HEADER + (uint64_t)len;
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

    /* Swapping the log invalidates every existing offset, so exclude lock-free
     * readers (they hold log_lock for read across their disk I/O) for the swap
     * and hash rebuild. Acquired while holding index_lock — order index->log. */
    pthread_rwlock_wrlock(&db->log_lock);
    log_close(&db->log);

    if (rename(newpath, db->path_log) != 0) {
        LOG_ERROR("compaction: rename %s -> %s failed; reopening original log",
                  newpath, db->path_log);
        /* try to reopen the original log so the server keeps working */
        log_open(&db->log, db->path_log,
                 config_effective_fsync_batch(&db->config));
        pthread_rwlock_unlock(&db->log_lock);
        free(locs);
        pthread_rwlock_unlock(&db->index_lock);
        return -1;
    }
    if (log_open(&db->log, db->path_log,
                 config_effective_fsync_batch(&db->config)) != 0) {
        LOG_ERROR("compaction: cannot reopen compacted log %s", db->path_log);
        pthread_rwlock_unlock(&db->log_lock);
        free(locs);
        pthread_rwlock_unlock(&db->index_lock);
        return -1;
    }

    /* Rebuild the hash with the new offsets (time/tag/sem indexes key by id, not
     * by log offset, so they remain valid). Tail tombstones are retained so a
     * record deleted mid-compaction is not resurrected by its phase-2 copy. */
    HashIndex *nh = hash_index_create();
    if (!nh) {
        pthread_rwlock_unlock(&db->log_lock);
        free(locs);
        pthread_rwlock_unlock(&db->index_lock);
        return -1;
    }
    for (size_t i = 0; i < nloc; i++)
        hash_index_put(nh, locs[i].id, locs[i].offset, locs[i].length,
                       locs[i].type, locs[i].deleted);
    hash_index_free(db->hash);
    db->hash = nh;
    pthread_rwlock_unlock(&db->log_lock);
    free(locs);

    /* The rewrite changed every log offset, so the on-disk checkpoints now carry
     * stale covered offsets. Invalidate both (hash + semantic); the next periodic
     * checkpoint (or clean shutdown) writes fresh ones. */
    unlink(db->path_index);
    unlink(db->path_sem);

    /* Count live entries from the rebuilt hash so duplicate ids (a phase-2 copy
     * superseded by a tail frame) are not double-counted. */
    size_t live = 0;
    for (size_t i = 0; i < nh->cap; i++)
        if (nh->buckets[i].used && !nh->buckets[i].deleted) live++;

    pthread_rwlock_unlock(&db->index_lock);
    LOG_INFO("compaction complete: %zu live records (%zu frames retained)", live,
             nloc);
    return 0;
}

struct Compactor {
    AegisDB *db;
    pthread_t thread;
    unsigned sweep_sec;
    unsigned compact_sec;
    atomic_int stop; /* relaxed: pthread_join sets up the shutdown happens-before */
};

/* Compaction rewrites the whole live log, so only run it once a meaningful
 * fraction of the log is dead (tombstones + superseded versions). Counting hash
 * tombstones is a cheap proxy under the read lock. Threshold: >= 25% dead. */
static int compaction_worthwhile(AegisDB *db) {
    pthread_rwlock_rdlock(&db->index_lock);
    size_t live = 0, dead = 0;
    const HashIndex *h = db->hash;
    for (size_t i = 0; i < h->cap; i++) {
        if (!h->buckets[i].used) continue;
        if (h->buckets[i].deleted) dead++;
        else live++;
    }
    pthread_rwlock_unlock(&db->index_lock);
    return dead > 0 && dead * 4 >= (live + dead);
}

static void *maint_loop(void *arg) {
    Compactor *c = arg;
    unsigned elapsed = 0;
    uint64_t last_fsync = db_now_ms();
    while (!atomic_load_explicit(&c->stop, memory_order_relaxed)) {
        sleep(1);
        if (atomic_load_explicit(&c->stop, memory_order_relaxed)) break;
        elapsed++;
        /* INTERVAL durability: flush the log on a time cadence so an idle
         * server cannot leave acknowledged writes unflushed indefinitely.
         * Resolution is bounded by the 1s tick. */
        if (c->db->config.durability == AEGIS_DURABILITY_INTERVAL &&
            log_flush_pending(&c->db->log)) {
            uint64_t now = db_now_ms();
            if (now - last_fsync >= c->db->config.fsync_interval_ms) {
                log_fsync(&c->db->log);
                last_fsync = now;
                LOG_DEBUG("interval fsync: flushed log to disk");
            }
        }
        if (c->sweep_sec && (elapsed % c->sweep_sec) == 0) {
            size_t swept = working_store_sweep(c->db->working, db_now_ms());
            if (swept)
                LOG_DEBUG("sweep: evicted %zu expired working-memory record(s)",
                          swept);
        }
        /* Periodically checkpoint the index so a crash recovers by replaying
         * only the log written since the last checkpoint, not the whole log. */
        if (c->db->config.checkpoint_sec &&
            (elapsed % c->db->config.checkpoint_sec) == 0) {
            if (db_checkpoint(c->db) == 0)
                LOG_DEBUG("checkpoint: index persisted");
            else
                LOG_WARN("checkpoint: failed to persist index");
        }
        if (c->compact_sec && (elapsed % c->compact_sec) == 0 &&
            compaction_worthwhile(c->db))
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
    atomic_store_explicit(&c->stop, 1, memory_order_relaxed);
    pthread_join(c->thread, NULL);
    free(c);
}