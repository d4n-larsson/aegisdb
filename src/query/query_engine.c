/* Query engine: operation router + core memory operations.
 *
 * T016 router; T020-T022/T026 (US1 insert/get/ping/errors); T029-T031 (US2
 * semantic insert/update + time/tag search); T037-T038 (US3 semantic search +
 * re-ranking); T043-T044 (US4 working insert/promote); T047-T049 (US5 relate/
 * traverse/agent scoping); T055 (phase gating). */
#include "aegisdb/query_engine.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aegisdb/compaction.h"
#include "aegisdb/json_request.h"
#include "aegisdb/json_response.h"
#include "aegisdb/logging.h"
#include "aegisdb/replication.h"
#include "aegisdb/sha256.h"

#include "qe_internal.h"

/* ----- phase gating (T055) --------------------------------------------- */

aegis_status_t require_phase(const AegisDB *db, int needed) {
    return (db->config.enabled_phase >= needed) ? AEGIS_OK
                                                : AEGIS_ERR_NOT_READY;
}

/* ----- validation ------------------------------------------------------- */

static int valid_tag(const char *t) {
    size_t n = strlen(t);
    if (n < 1 || n > 64) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* A client-supplied agent_id (namespace) is only reachable with no auth or an
 * admin token — a namespaced token pins it. Bound its length and reject control
 * characters (which could corrupt logs or the token file's line format);
 * otherwise permissive, since operator-defined namespaces vary. 1..128 bytes. */
static int valid_agent_id(const char *a) {
    size_t n = strlen(a);
    if (n < 1 || n > MAX_AGENT_ID) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)a[i];
        if (c < 0x20 || c == 0x7f) return 0; /* no control chars */
    }
    return 1;
}

static aegis_status_t validate_common(AegisDB *db, const MemoryRecord *r) {
    if (r->data_len > db->config.max_payload_bytes)
        return AEGIS_ERR_PAYLOAD_TOO_LARGE;
    if (r->importance < 0.0f || r->importance > 1.0f)
        return AEGIS_ERR_INVALID_REQUEST;
    if (r->confidence < 0.0f || r->confidence > 1.0f)
        return AEGIS_ERR_INVALID_REQUEST;
    if (r->tag_count > MAX_TAGS) return AEGIS_ERR_INVALID_REQUEST;
    for (size_t i = 0; i < r->tag_count; i++)
        if (!valid_tag(r->tags[i])) return AEGIS_ERR_INVALID_REQUEST;
    if (r->agent_id && !valid_agent_id(r->agent_id))
        return AEGIS_ERR_INVALID_REQUEST;
    if (r->embedding_dim &&
        r->embedding_dim != db->config.embedding_dimensions)
        return AEGIS_ERR_INVALID_REQUEST;
    return AEGIS_OK;
}

/* ----- low-level persistence (caller holds index write lock) ------------ */

static aegis_status_t append_and_hash(AegisDB *db, const MemoryRecord *rec) {
    uint8_t *buf = NULL;
    size_t len = 0;
    if (record_encode(rec, &buf, &len) != 0) return AEGIS_ERR_INTERNAL;

    /* Per-tenant accounting (this is the sole live-set mutation point, so it is
     * also the sole accounting point). Only under auth — with no namespaces
     * there are no tenants to bound. The delta vs the record's current live
     * state covers insert (+1), delete/tombstone (-1), and update (bytes only,
     * count unchanged). agent_id is immutable per id, so it is the tenant. */
    const char *ns =
        (db->config.auth_token_count > 0) ? rec->agent_id : NULL;
    long d_records = 0, d_bytes = 0;
    if (ns) {
        const HashEntry *prior = hash_index_get(db->hash, rec->id);
        int prior_live = prior && !prior->deleted;
        int new_live = !rec->deleted;
        long prior_bytes = prior_live ? (long)prior->length : 0;
        long new_bytes = new_live ? (long)len : 0;
        d_records = (new_live ? 1 : 0) - (prior_live ? 1 : 0);
        d_bytes = new_bytes - prior_bytes;
        /* Reject a write that would push this tenant over quota — but never a
         * delete/shrink (checked on positive deltas only). */
        if (tenant_usage_would_exceed(
                db->tenants, ns, d_records > 0 ? d_records : 0,
                d_bytes > 0 ? d_bytes : 0, db->config.tenant_max_records,
                db->config.tenant_max_bytes) != 0) {
            free(buf);
            return AEGIS_ERR_QUOTA_EXCEEDED;
        }
    }

    uint64_t off = 0;
    int rv = log_append(&db->log, buf, len, &off);
    free(buf);
    if (rv != 0) return AEGIS_ERR_INTERNAL;
    if (hash_index_put(db->hash, rec->id, off, (uint32_t)len, (uint8_t)rec->type,
                       (uint8_t)(rec->deleted ? 1 : 0), rec->expires_at) != 0)
        return AEGIS_ERR_INTERNAL;
    if (ns && (d_records || d_bytes))
        tenant_usage_adjust(db->tenants, ns, d_records, d_bytes);
    return AEGIS_OK;
}

/* Read + decode a live persisted record (caller holds at least a read lock). */
static aegis_status_t load_record(AegisDB *db, uint64_t id, MemoryRecord *out) {
    const HashEntry *e = hash_index_get(db->hash, id);
    if (!e) return AEGIS_ERR_NOT_FOUND;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (log_read(&db->log, e->offset, &buf, &len) != 0)
        return AEGIS_ERR_INTERNAL;
    int rv = record_decode(buf, len, out);
    free(buf);
    return rv == 0 ? AEGIS_OK : AEGIS_ERR_INTERNAL;
}

/* Read + decode a record for a read-only operation, keeping the disk I/O off
 * the index lock: resolve the id -> log offset under index_lock (read), pin the
 * log against compaction with log_lock (read), drop index_lock, then read. So
 * writers — which take index_lock for write but never log_lock — are not
 * blocked by the read's I/O. Caller must hold neither lock. */
static aegis_status_t load_record_ro(AegisDB *db, uint64_t id,
                                     MemoryRecord *out) {
    pthread_rwlock_rdlock(&db->index_lock);
    const HashEntry *e = hash_index_get(db->hash, id);
    if (!e) {
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_NOT_FOUND;
    }
    uint64_t off = e->offset;
    pthread_rwlock_rdlock(&db->log_lock);
    pthread_rwlock_unlock(&db->index_lock);
    uint8_t *buf = NULL;
    size_t len = 0;
    int rv = log_read(&db->log, off, &buf, &len);
    pthread_rwlock_unlock(&db->log_lock);
    if (rv != 0) return AEGIS_ERR_INTERNAL;
    rv = record_decode(buf, len, out);
    free(buf);
    return rv == 0 ? AEGIS_OK : AEGIS_ERR_INTERNAL;
}

/* Cross-tenant guard: when ns is set, a record whose agent_id the caller's
 * namespace does not own (different or absent) must be indistinguishable from
 * missing. agent_id is immutable per id, so checking it on the already-loaded
 * record is equivalent to the old separate ownership pre-read. */
int ns_denies(const char *ns, const MemoryRecord *r) {
    return ns && (!r->agent_id || strcmp(r->agent_id, ns) != 0);
}

/* A TTL'd record past its horizon is archived: hidden from recall (get/search/
 * traverse) until the expiry sweep tombstones it. */
int record_expired(const MemoryRecord *r, uint64_t now) {
    return r->expires_at != 0 && now >= r->expires_at;
}

/* ----- core operations -------------------------------------------------- */

aegis_status_t qe_insert(AegisDB *db, const MemoryRecord *in,
                         const char *session_id, uint64_t ttl_ms,
                         MemoryRecord *out) {
    aegis_status_t st = validate_common(db, in);
    if (st != AEGIS_OK) return st;
    if (in->data_len == 0) return AEGIS_ERR_INVALID_REQUEST;

    if (in->type == MEM_WORKING) {
        st = require_phase(db, 4);
        if (st != AEGIS_OK) return st;
        if (!session_id) return AEGIS_ERR_INVALID_REQUEST;
        uint64_t now = db_now_ms();
        uint64_t wid = 0;
        if (working_store_add(db->working, session_id, in, now, ttl_ms, &wid) !=
            0)
            return AEGIS_ERR_INTERNAL;
        MemoryRecord *got = working_store_get(db->working, session_id, wid, now);
        if (!got) return AEGIS_ERR_INTERNAL;
        *out = *got;
        free(got);
        return AEGIS_OK;
    }

    if (in->type == MEM_EPISODIC) {
        st = require_phase(db, 1);
    } else if (in->type == MEM_SEMANTIC) {
        st = require_phase(db, 2);
    } else {
        return AEGIS_ERR_INVALID_REQUEST;
    }
    if (st != AEGIS_OK) return st;

    /* Backpressure: refuse a persisted insert once index RAM reaches the cap, so
     * a growing dataset returns MEMORY_LIMIT instead of getting OOM-killed. The
     * sampled value (maintenance thread) is read lock-free; deletes/updates still
     * proceed so memory can be freed. Working memory (above) is a bounded ring
     * and is exempt. */
    if (db->config.max_index_bytes &&
        atomic_load_explicit(&db->index_bytes, memory_order_relaxed) >=
            db->config.max_index_bytes)
        return AEGIS_ERR_MEMORY_LIMIT;

    MemoryRecord *rec = record_clone(in);
    if (!rec) return AEGIS_ERR_INTERNAL;
    uint64_t now = db_now_ms();
    rec->id = db_next_id(db);
    rec->created = now;
    rec->updated = now; /* episodic: updated == created */
    rec->deleted = 0;
    /* Opt-in TTL (#73): a positive ttl_ms archives the record after the horizon
     * — hidden from recall immediately, reclaimed by the expiry sweep. 0 (the
     * default) means never, preserving the durable/audit-log behaviour. A huge
     * ttl_ms is saturated to "never" rather than wrapping now+ttl_ms into the
     * past (which would make the record expire immediately). */
    rec->expires_at =
        ttl_ms ? (ttl_ms > UINT64_MAX - now ? UINT64_MAX : now + ttl_ms) : 0;

    pthread_rwlock_wrlock(&db->index_lock);
    st = append_and_hash(db, rec);
    if (st == AEGIS_OK) {
        time_index_add(db->time, rec->created, rec->id);
        for (size_t i = 0; i < rec->tag_count; i++)
            tag_index_add(db->tags, rec->tags[i], rec->id);
        if (rec->embedding_dim && rec->vec_count)
            semantic_index_add(db->sem, rec->id, rec->embedding, rec->vec_count,
                               rec->embedding_dim);
    }
    pthread_rwlock_unlock(&db->index_lock);
    if (st == AEGIS_OK && log_fsync_if_batched(&db->log) != 0)
        st = AEGIS_ERR_INTERNAL; /* not durable: do not acknowledge the write */

    if (st != AEGIS_OK) {
        record_free(rec);
        free(rec);
        return st;
    }
    *out = *rec; /* move ownership */
    free(rec);
    return AEGIS_OK;
}

aegis_status_t qe_get(AegisDB *db, uint64_t id, const char *agent_filter,
                      MemoryRecord *out) {
    aegis_status_t st = load_record_ro(db, id, out);
    if (st != AEGIS_OK) return st;
    if (out->deleted || record_expired(out, db_now_ms())) {
        record_free(out);
        return AEGIS_ERR_NOT_FOUND;
    }
    if (agent_filter && (!out->agent_id ||
                         strcmp(out->agent_id, agent_filter) != 0)) {
        record_free(out);
        return AEGIS_ERR_NOT_FOUND;
    }
    return AEGIS_OK;
}

aegis_status_t qe_update(AegisDB *db, uint64_t id, const UpdatePatch *patch,
                         const char *ns, MemoryRecord *out) {
    aegis_status_t st = require_phase(db, 2);
    if (st != AEGIS_OK) return st;

    pthread_rwlock_wrlock(&db->index_lock);
    MemoryRecord cur;
    st = load_record(db, id, &cur);
    if (st != AEGIS_OK) {
        pthread_rwlock_unlock(&db->index_lock);
        return st;
    }
    /* A record outside the caller's namespace reads back as missing, checked
     * before the type check so it cannot leak via an IMMUTABLE response. */
    if (cur.deleted || ns_denies(ns, &cur)) {
        record_free(&cur);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_NOT_FOUND;
    }
    if (cur.type != MEM_SEMANTIC) {
        record_free(&cur);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_IMMUTABLE;
    }

    /* old tags for index diff */
    if (patch->has_data) {
        void *nd = NULL;
        if (patch->data_len) {
            nd = malloc(patch->data_len);
            if (!nd) {
                record_free(&cur);
                pthread_rwlock_unlock(&db->index_lock);
                return AEGIS_ERR_INTERNAL;
            }
            memcpy(nd, patch->data, patch->data_len);
        }
        free(cur.data);
        cur.data = nd;
        cur.data_len = patch->data_len;
    }
    if (patch->has_importance) cur.importance = patch->importance;
    if (patch->has_confidence) cur.confidence = patch->confidence;

    /* Stage the new tags on the record, but defer the shared tag-index mutation
     * until validate_common + append_and_hash both succeed. The old ordering
     * rewrote the index up front, so a rejected update (e.g. an out-of-range
     * importance) or a failed append left the index desynced from the log — the
     * record's real tags — making live records unsearchable by their tags until
     * a restart rebuilt the index from the log. */
    char **old_tags = NULL;
    size_t old_tag_count = 0;
    if (patch->has_tags) {
        old_tags = cur.tags; /* detach so record_set_tags won't free them */
        old_tag_count = cur.tag_count;
        cur.tags = NULL;
        cur.tag_count = 0;
        if (record_set_tags(&cur, patch->tags, patch->tag_count) != 0) {
            cur.tags = old_tags; /* reattach originals for record_free */
            cur.tag_count = old_tag_count;
            record_free(&cur);
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
    }

    cur.updated = db_now_ms();
    st = validate_common(db, &cur);
    if (st == AEGIS_OK) st = append_and_hash(db, &cur);
    if (st == AEGIS_OK && patch->has_tags) {
        /* Committed: now it's safe to swing the tag index from old to new. */
        for (size_t i = 0; i < old_tag_count; i++)
            tag_index_remove(db->tags, old_tags[i], cur.id);
        for (size_t i = 0; i < cur.tag_count; i++)
            tag_index_add(db->tags, cur.tags[i], cur.id);
    }
    pthread_rwlock_unlock(&db->index_lock);
    if (st == AEGIS_OK && log_fsync_if_batched(&db->log) != 0)
        st = AEGIS_ERR_INTERNAL; /* not durable: do not acknowledge the write */

    for (size_t i = 0; i < old_tag_count; i++) free(old_tags[i]);
    free(old_tags);

    if (st != AEGIS_OK) {
        record_free(&cur);
        return st;
    }
    *out = cur; /* move ownership */
    return AEGIS_OK;
}

aegis_status_t qe_delete(AegisDB *db, uint64_t id, const char *ns) {
    aegis_status_t st = require_phase(db, 1);
    if (st != AEGIS_OK) return st;

    pthread_rwlock_wrlock(&db->index_lock);
    MemoryRecord cur;
    st = load_record(db, id, &cur);
    if (st != AEGIS_OK) {
        pthread_rwlock_unlock(&db->index_lock);
        return st;
    }
    if (cur.deleted || ns_denies(ns, &cur)) {
        record_free(&cur);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_NOT_FOUND;
    }

    /* drop from secondary indexes so it stops surfacing in queries */
    for (size_t i = 0; i < cur.tag_count; i++)
        tag_index_remove(db->tags, cur.tags[i], cur.id);
    if (cur.embedding_dim)
        semantic_index_remove(db->sem, cur.id);
    time_index_remove(db->time, cur.created, cur.id);

    cur.deleted = 1;
    cur.updated = db_now_ms();
    st = append_and_hash(db, &cur); /* tombstone version; hash marks deleted */
    pthread_rwlock_unlock(&db->index_lock);
    if (st == AEGIS_OK && log_fsync_if_batched(&db->log) != 0)
        st = AEGIS_ERR_INTERNAL; /* not durable: do not acknowledge the write */

    record_free(&cur);
    return st;
}

aegis_status_t qe_promote(AegisDB *db, const char *session_id,
                          uint64_t working_id, MemoryType to_type,
                          const char *ns, MemoryRecord *out) {
    aegis_status_t st = require_phase(db, 4);
    if (st != AEGIS_OK) return st;
    if (!session_id) return AEGIS_ERR_INVALID_REQUEST;
    if (to_type != MEM_EPISODIC && to_type != MEM_SEMANTIC)
        return AEGIS_ERR_INVALID_REQUEST;

    MemoryRecord w;
    if (working_store_take(db->working, session_id, working_id, db_now_ms(), ns,
                           &w) != 0)
        return AEGIS_ERR_NOT_FOUND;

    /* re-insert as a persisted record, pinned to the caller's namespace */
    w.type = to_type;
    w.expires_at = 0;
    if (ns) {
        free(w.agent_id);
        w.agent_id = strdup(ns);
        if (!w.agent_id) {
            record_free(&w);
            return AEGIS_ERR_INTERNAL;
        }
    }
    st = qe_insert(db, &w, NULL, 0, out);
    record_free(&w);
    return st;
}

/* Two relationships are the same edge when they point at the same target with
 * the same kind (kind may be NULL). */
static int rel_same(const Relationship *e, uint64_t to_id, const char *kind) {
    if (e->to_id != to_id) return 0;
    if (!e->kind || !kind) return e->kind == kind; /* both NULL == equal */
    return strcmp(e->kind, kind) == 0;
}

aegis_status_t qe_relate(AegisDB *db, uint64_t from_id, uint64_t to_id,
                         const char *kind, const char *ns) {
    aegis_status_t st = require_phase(db, 4);
    if (st != AEGIS_OK) return st;
    /* A self-edge carries no graph information and would still consume a slot. */
    if (from_id == to_id) return AEGIS_ERR_INVALID_REQUEST;

    pthread_rwlock_wrlock(&db->index_lock);
    MemoryRecord from;
    st = load_record(db, from_id, &from);
    if (st != AEGIS_OK || from.deleted || ns_denies(ns, &from)) {
        if (st == AEGIS_OK) record_free(&from);
        pthread_rwlock_unlock(&db->index_lock);
        return st == AEGIS_OK ? AEGIS_ERR_NOT_FOUND : st;
    }
    /* The target must exist; a namespaced caller must additionally own it, so it
     * is loaded (not just existence-checked) to verify its namespace. Both cases
     * collapse to NOT_FOUND so neither leaks across tenants. */
    if (ns) {
        MemoryRecord to_rec;
        aegis_status_t tst = load_record(db, to_id, &to_rec);
        if (tst != AEGIS_OK || to_rec.deleted || ns_denies(ns, &to_rec)) {
            if (tst == AEGIS_OK) record_free(&to_rec);
            record_free(&from);
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_NOT_FOUND;
        }
        record_free(&to_rec);
    } else if (!hash_index_get(db->hash, to_id)) {
        record_free(&from);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_NOT_FOUND;
    }
    /* Idempotent: an identical (to_id, kind) edge already present is a no-op, so
     * repeated relate calls cannot grow the record without bound. */
    for (size_t i = 0; i < from.rel_count; i++) {
        if (rel_same(&from.relationships[i], to_id, kind)) {
            record_free(&from);
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_OK;
        }
    }
    /* Hard cap keeps rel_count well under the u16 wire limit (see record_encode). */
    if (from.rel_count >= MAX_RELATIONSHIPS) {
        record_free(&from);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_QUOTA_EXCEEDED;
    }
    if (record_add_relationship(&from, from_id, to_id, kind) != 0) {
        record_free(&from);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_INTERNAL;
    }
    from.updated = db_now_ms();
    st = append_and_hash(db, &from); /* relationship metadata, content intact */
    pthread_rwlock_unlock(&db->index_lock);
    if (st == AEGIS_OK && log_fsync_if_batched(&db->log) != 0)
        st = AEGIS_ERR_INTERNAL; /* not durable: do not acknowledge the write */
    record_free(&from);
    return st;
}

aegis_status_t qe_traverse(AegisDB *db, uint64_t start_id, int depth,
                           const char *agent_filter, MemoryRecord **out,
                           size_t *out_n) {
    aegis_status_t st = require_phase(db, 4);
    if (st != AEGIS_OK) return st;
    if (depth < 0) depth = 0;
    uint64_t now = db_now_ms(); /* for expiry, sampled once for the whole walk */

    /* BFS over relationship edges */
    uint64_t *seen = NULL;
    size_t seen_n = 0, seen_cap = 0;
    uint64_t *frontier = malloc(sizeof(uint64_t));
    size_t front_n = 0;
    if (!frontier) return AEGIS_ERR_INTERNAL;
    frontier[front_n++] = start_id;

    Cand *acc = NULL;
    size_t acc_n = 0, acc_cap = 0;

    /* On any allocation failure we stop growing and return what we have so far
     * (like the malloc(offs) guard below), rather than dereferencing a failed
     * realloc's NULL or leaking the old buffer it left untouched. */
    int oom = 0;
    for (int level = 0; level <= depth && front_n > 0 && !oom; level++) {
        /* Resolve this level's not-yet-seen ids to log offsets under the index
         * lock, then read+decode them off it (disk I/O under log_lock only). */
        uint64_t *offs = malloc(front_n * sizeof(uint64_t));
        if (!offs) break; /* frontier freed after the loop; return what we have */
        size_t off_n = 0;

        pthread_rwlock_rdlock(&db->index_lock);
        for (size_t i = 0; i < front_n; i++) {
            uint64_t id = frontier[i];
            int dup = 0;
            for (size_t s = 0; s < seen_n; s++)
                if (seen[s] == id) { dup = 1; break; }
            if (dup) continue;
            if (seen_n == seen_cap) {
                size_t nc = seen_cap ? seen_cap * 2 : 8;
                uint64_t *tmp = realloc(seen, nc * sizeof(uint64_t));
                if (!tmp) { oom = 1; break; }
                seen = tmp;
                seen_cap = nc;
            }
            seen[seen_n++] = id;
            const HashEntry *e = hash_index_get(db->hash, id);
            if (!e) continue;
            offs[off_n++] = e->offset;
        }
        pthread_rwlock_rdlock(&db->log_lock);
        pthread_rwlock_unlock(&db->index_lock);
        free(frontier);
        frontier = NULL;

        uint64_t *next = NULL;
        size_t next_n = 0, next_cap = 0;
        for (size_t i = 0; i < off_n && !oom; i++) {
            uint8_t *buf = NULL;
            size_t len = 0;
            if (log_read(&db->log, offs[i], &buf, &len) != 0) continue;
            MemoryRecord r;
            int dec = record_decode(buf, len, &r);
            free(buf);
            if (dec != 0) continue;
            if (r.deleted || record_expired(&r, now) ||
                (agent_filter && (!r.agent_id ||
                                  strcmp(r.agent_id, agent_filter) != 0))) {
                /* a filtered/expired node is skipped entirely, edges and all */
                record_free(&r);
                continue;
            }
            /* collect */
            if (acc_n == acc_cap) {
                size_t nc = acc_cap ? acc_cap * 2 : 8;
                Cand *tmp = realloc(acc, nc * sizeof(Cand));
                if (!tmp) { record_free(&r); oom = 1; break; }
                acc = tmp;
                acc_cap = nc;
            }
            acc[acc_n].rec = r; /* keep; do not free */
            acc[acc_n].score = 0;
            acc_n++;
            /* enqueue neighbours */
            for (size_t k = 0; k < r.rel_count; k++) {
                if (next_n == next_cap) {
                    size_t nc = next_cap ? next_cap * 2 : 8;
                    uint64_t *tmp = realloc(next, nc * sizeof(uint64_t));
                    if (!tmp) { oom = 1; break; }
                    next = tmp;
                    next_cap = nc;
                }
                next[next_n++] = r.relationships[k].to_id;
            }
        }
        pthread_rwlock_unlock(&db->log_lock);
        free(offs);
        frontier = next;
        front_n = next_n;
        (void)next_cap;
    }
    free(frontier);
    free(seen);

    MemoryRecord *res = malloc((acc_n ? acc_n : 1) * sizeof(MemoryRecord));
    if (!res) {
        for (size_t i = 0; i < acc_n; i++) record_free(&acc[i].rec);
        free(acc);
        return AEGIS_ERR_INTERNAL;
    }
    for (size_t i = 0; i < acc_n; i++) res[i] = acc[i].rec;
    free(acc);
    *out = res;
    *out_n = acc_n;
    return AEGIS_OK;
}
