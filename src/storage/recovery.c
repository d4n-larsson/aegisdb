/* Startup recovery: rebuild indexes from the log (T013, T034, T040). */
#include "aegisdb/recovery.h"

#include <stdio.h>
#include <stdlib.h>

#include "aegisdb/hash_index.h"
#include "aegisdb/logging.h"
#include "aegisdb/record.h"

typedef struct {
    AegisDB *db;
    uint64_t max_id;
} ScanCtx;

/* reconcile predicate: keep a semantic id only while it's a live hash entry */
static int recover_keep(uint64_t id, void *ctx) {
    return hash_index_get((const HashIndex *)ctx, id) != NULL;
}

/* Pass 1: build the hash index (latest frame per id wins) and track max id. */
static int scan_cb(uint64_t offset, const uint8_t *payload, size_t len,
                   void *ctx) {
    ScanCtx *sc = ctx;
    MemoryRecord r;
    if (record_decode(payload, len, &r) != 0)
        return 0; /* skip undecodable frame, keep scanning */
    if (r.id > sc->max_id) sc->max_id = r.id;
    hash_index_put(sc->db->hash, r.id, offset, (uint32_t)len, (uint8_t)r.type,
                   (uint8_t)(r.deleted ? 1 : 0), r.expires_at);
    record_free(&r);
    return 0;
}

long recovery_run(AegisDB *db) {
    /* Try the checkpoint: it maps id -> log location for everything up to its
     * covered offset, so we can trust [0, covered) and scan only the tail. A
     * missing, corrupt, or stale (covers more than the log holds) checkpoint
     * falls back to a full scan from offset 0. */
    uint64_t covered = 0, snap_next_id = 0;
    int have_checkpoint = 0;
    if (hash_index_load(db->hash, db->path_index, &covered, &snap_next_id) == 0) {
        if (covered <= (uint64_t)db->log.size) {
            have_checkpoint = 1;
            LOG_DEBUG("recovery: loaded checkpoint (%zu entries, covers %llu of "
                      "%llu bytes)",
                      db->hash->count, (unsigned long long)covered,
                      (unsigned long long)db->log.size);
        } else {
            LOG_WARN("recovery: checkpoint covers %llu bytes but log holds only "
                     "%llu; ignoring it and rebuilding from the log",
                     (unsigned long long)covered,
                     (unsigned long long)db->log.size);
            hash_index_free(db->hash);
            db->hash = hash_index_create();
            if (!db->hash) return -1;
        }
    }

    /* Seed max_id from the checkpoint so an empty tail still yields the right
     * next_id. */
    ScanCtx sc = {db, have_checkpoint && snap_next_id ? snap_next_id - 1 : 0};
    uint64_t scan_from = have_checkpoint ? covered : 0;
    LogScanResult res = {0};
    LOG_DEBUG("recovery: scanning log %s from %llu (%llu bytes total)",
              db->path_log, (unsigned long long)scan_from,
              (unsigned long long)db->log.size);
    if (log_scan(&db->log, scan_from, scan_cb, &sc, &res) != 0) {
        LOG_ERROR("recovery: log scan failed on %s", db->path_log);
        return -1;
    }

    /* Corruption in the middle of the log: the bad frames were skipped and the
     * surrounding records recovered. Surface it loudly — this is data loss. */
    if (res.corrupt_frames > 0)
        LOG_ERROR("recovery: skipped %zu corrupt frame(s); %s",
                  res.corrupt_frames,
                  res.recovered_after_hole
                      ? "recovered records past the damage (log left intact)"
                      : "damage was at the tail");

    /* Drop a torn tail left by a mid-write crash. */
    if (res.truncate_to < (uint64_t)db->log.size) {
        LOG_WARN("truncating torn tail: %llu -> %llu bytes",
                 (unsigned long long)db->log.size,
                 (unsigned long long)res.truncate_to);
        log_truncate(&db->log, res.truncate_to);
    }

    db->next_id = sc.max_id + 1;
    LOG_DEBUG("recovery: hash index rebuilt, %zu entries, next id %llu",
              db->hash->count, (unsigned long long)db->next_id);

    /* Try the semantic (HNSW) checkpoint: it carries the live vectors and the
     * graph as of its covered offset, so we skip the O(n log n) graph rebuild
     * and only replay the semantic tail. A missing/corrupt/stale checkpoint
     * leaves the index empty and falls back to a full rebuild in pass 2. */
    uint64_t sem_covered = 0;
    int sem_loaded = 0;
    if (semantic_index_load(db->sem, db->path_sem, &sem_covered) == 0) {
        if (sem_covered <= (uint64_t)db->log.size) {
            sem_loaded = 1;
            LOG_DEBUG("recovery: loaded semantic checkpoint (%zu vectors, covers "
                      "%llu bytes)",
                      semantic_index_count(db->sem),
                      (unsigned long long)sem_covered);
        } else {
            LOG_WARN("recovery: semantic checkpoint covers %llu bytes but log "
                     "holds only %llu; ignoring it",
                     (unsigned long long)sem_covered,
                     (unsigned long long)db->log.size);
            semantic_index_clear(db->sem);
        }
    }

    /* Pass 2: populate secondary indexes from the surviving (latest) records.
     * time/tag are always rebuilt; semantic is rebuilt fully unless a checkpoint
     * loaded, in which case only the tail (offset >= sem_covered) is applied. */
    long live = 0;
    HashIndex *h = db->hash;
    for (size_t i = 0; i < h->cap; i++) {
        const HashEntry *e = &h->buckets[i];
        if (!e->used || e->deleted) continue;
        uint8_t *buf = NULL;
        size_t blen = 0;
        if (log_read(&db->log, e->offset, &buf, &blen) != 0) continue;
        MemoryRecord r;
        if (record_decode(buf, blen, &r) == 0) {
            time_index_add(db->time, r.created, r.id);
            for (size_t k = 0; k < r.tag_count; k++)
                tag_index_add(db->tags, r.tags[k], r.id);
            /* Seed per-tenant usage from the surviving live set so quotas are
             * accurate immediately after a restart (matches append_and_hash's
             * accounting unit: +1 record, +frame-payload bytes). Only under
             * auth, where namespaces exist. */
            if (db->config.auth_token_count > 0 && r.agent_id)
                tenant_usage_adjust(db->tenants, r.agent_id, 1, (long)blen);
            if (r.embedding_dim == db->config.embedding_dimensions &&
                r.embedding && r.vec_count &&
                (!sem_loaded || e->offset >= sem_covered))
                semantic_index_add(db->sem, r.id, r.embedding, r.vec_count,
                                   r.embedding_dim);
            record_free(&r);
            live++;
        }
        free(buf);
    }

    /* The loaded graph reflects the checkpoint's covered prefix and cannot see
     * records deleted in the tail (pass 2 skips deleted hash entries), so evict
     * any semantic id the final hash no longer reports live. */
    if (sem_loaded) semantic_index_reconcile(db->sem, recover_keep, db->hash);

    LOG_DEBUG("recovery: secondary indexes populated from %ld live record(s)",
              live);
    return live;
}