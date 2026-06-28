/* Startup recovery: rebuild indexes from the log (T013, T034, T040). */
#include "aegisdb/recovery.h"

#include <stdio.h>
#include <stdlib.h>

#include "aegisdb/logging.h"
#include "aegisdb/record.h"

typedef struct {
    AegisDB *db;
    uint64_t max_id;
} ScanCtx;

/* Pass 1: build the hash index (latest frame per id wins) and track max id. */
static int scan_cb(uint64_t offset, const uint8_t *payload, size_t len,
                   void *ctx) {
    ScanCtx *sc = ctx;
    MemoryRecord r;
    if (record_decode(payload, len, &r) != 0)
        return 0; /* skip undecodable frame, keep scanning */
    if (r.id > sc->max_id) sc->max_id = r.id;
    hash_index_put(sc->db->hash, r.id, offset, (uint32_t)len, (uint8_t)r.type,
                   (uint8_t)(r.deleted ? 1 : 0));
    record_free(&r);
    return 0;
}

long recovery_run(AegisDB *db) {
    ScanCtx sc = {db, 0};
    uint64_t valid_end = 0;
    LOG_DEBUG("recovery: scanning log %s (%llu bytes)", db->path_log,
              (unsigned long long)db->log.size);
    if (log_scan(&db->log, scan_cb, &sc, &valid_end) != 0) {
        LOG_ERROR("recovery: log scan failed on %s", db->path_log);
        return -1;
    }

    /* Drop a torn tail left by a mid-write crash. */
    if (valid_end < (uint64_t)db->log.size) {
        LOG_WARN("truncating torn tail: %llu -> %llu bytes",
                 (unsigned long long)db->log.size,
                 (unsigned long long)valid_end);
        log_truncate(&db->log, valid_end);
    }

    db->next_id = sc.max_id + 1;
    LOG_DEBUG("recovery: hash index rebuilt, %zu entries, next id %llu",
              db->hash->count, (unsigned long long)db->next_id);

    /* Pass 2: populate secondary indexes from the surviving (latest) records. */
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
            if (r.embedding_dim == db->config.embedding_dimensions &&
                r.embedding)
                semantic_index_add(db->sem, r.id, r.embedding, r.embedding_dim);
            record_free(&r);
            live++;
        }
        free(buf);
    }
    LOG_DEBUG("recovery: secondary indexes populated from %ld live record(s)",
              live);
    return live;
}