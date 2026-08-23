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
#include "aegisdb/hash_mix.h"
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
    if (n < 1 || n > 64) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

/* A client-supplied agent_id (namespace) is only reachable with no auth or an
 * admin token — a namespaced token pins it. Bound its length and reject control
 * characters (which could corrupt logs or the token file's line format);
 * otherwise permissive, since operator-defined namespaces vary. 1..128 bytes. */
static int valid_agent_id(const char *a) {
    size_t n = strlen(a);
    if (n < 1 || n > MAX_AGENT_ID) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)a[i];
        if (c < 0x20 || c == 0x7f) {
            return 0; /* no control chars */
        }
    }
    return 1;
}

static aegis_status_t validate_common(AegisDB *db, const MemoryRecord *r) {
    if (r->data_len > db->config.max_payload_bytes) {
        return AEGIS_ERR_PAYLOAD_TOO_LARGE;
    }
    if (r->importance < 0.0F || r->importance > 1.0F) {
        return AEGIS_ERR_INVALID_REQUEST;
    }
    if (r->confidence < 0.0F || r->confidence > 1.0F) {
        return AEGIS_ERR_INVALID_REQUEST;
    }
    if (r->tag_count > MAX_TAGS) {
        return AEGIS_ERR_INVALID_REQUEST;
    }
    for (size_t i = 0; i < r->tag_count; i++) {
        if (!valid_tag(r->tags[i])) {
            return AEGIS_ERR_INVALID_REQUEST;
        }
    }
    if (r->agent_id && !valid_agent_id(r->agent_id)) {
        return AEGIS_ERR_INVALID_REQUEST;
    }
    if (r->embedding_dim &&
        r->embedding_dim != db->config.embedding_dimensions) {
        return AEGIS_ERR_INVALID_REQUEST;
    }
    return AEGIS_OK;
}

/* ----- low-level persistence (caller holds index write lock) ------------ */

static aegis_status_t append_and_hash(AegisDB *db, const MemoryRecord *rec) {
    uint8_t *buf = NULL;
    size_t len = 0;
    if (record_encode(rec, &buf, &len) != 0) {
        /* Usually a refusal rather than an allocation failure: encode rejects a
         * record it has no defined encoding for — an unknown fact kind or
         * derivation rule, a derivation with no fact to explain, a count past
         * its wire width. All of those surface to the caller as a bare
         * INTERNAL, so without this line the write is dropped with nothing to
         * diagnose it by. */
        LOG_ERROR("record_encode refused record %llu (type %d, fact kind %d, "
                  "derivation routes %d); write dropped",
                  (unsigned long long)rec->id, (int)rec->type,
                  (int)rec->fact.kind, (int)rec->derivation.route_count);
        return AEGIS_ERR_INTERNAL;
    }

    /* Per-tenant accounting (this is the sole live-set mutation point, so it is
     * also the sole accounting point). Only under auth — with no namespaces
     * there are no tenants to bound. The delta vs the record's current live
     * state covers insert (+1), delete/tombstone (-1), and update (bytes only,
     * count unchanged). agent_id is immutable per id, so it is the tenant. */
    /* auth_token_count can change under auth_lock (runtime token add/revoke), so
     * read it under auth_lock(read) rather than racing that writer. The caller
     * holds index_lock(write); auth_lock nests under it (nothing takes auth_lock
     * then index_lock), so this adds no cycle. */
    pthread_rwlock_rdlock(&db->auth_lock);
    int auth_on = db->config.auth_token_count > 0;
    pthread_rwlock_unlock(&db->auth_lock);
    const char *ns = auth_on ? rec->agent_id : NULL;
    long d_records = 0;
    long d_bytes = 0;
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
    if (rv != 0) {
        return AEGIS_ERR_INTERNAL;
    }
    if (hash_index_put(db->hash, rec->id, off, (uint32_t)len,
                       (uint8_t)rec->type, (uint8_t)(rec->deleted ? 1 : 0),
                       rec->expires_at) != 0) {
        return AEGIS_ERR_INTERNAL;
    }
    if (ns && (d_records || d_bytes)) {
        tenant_usage_adjust(db->tenants, ns, d_records, d_bytes);
    }
    return AEGIS_OK;
}

/* Read + decode a live persisted record (caller holds at least a read lock). */
static aegis_status_t load_record(AegisDB *db, uint64_t id, MemoryRecord *out) {
    const HashEntry *e = hash_index_get(db->hash, id);
    if (!e) {
        return AEGIS_ERR_NOT_FOUND;
    }
    uint8_t *buf = NULL;
    size_t len = 0;
    if (log_read(&db->log, e->offset, &buf, &len) != 0) {
        return AEGIS_ERR_INTERNAL;
    }
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
    if (rv != 0) {
        return AEGIS_ERR_INTERNAL;
    }
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

/* Durability fsync for the write path. Called after releasing index_lock (so
 * the fsync is off that lock), but under log_lock(read) so a concurrent
 * compaction swap — which closes and reopens db->log under log_lock(write),
 * destroying the log's mutex and memset-ing the struct — cannot run underneath
 * it. Taking log_lock alone here respects the index->log order (index_lock is
 * already released). Returns 0, or -1 if the write is not durable. */
static int durably_flush(AegisDB *db) {
    pthread_rwlock_rdlock(&db->log_lock);
    int rv = log_fsync_if_batched(&db->log);
    pthread_rwlock_unlock(&db->log_lock);
    return rv;
}

/* ----- core operations -------------------------------------------------- */

aegis_status_t qe_insert(AegisDB *db, const MemoryRecord *in,
                         const char *session_id, uint64_t ttl_ms,
                         MemoryRecord *out) {
    aegis_status_t st = validate_common(db, in);
    if (st != AEGIS_OK) {
        return st;
    }
    if (in->data_len == 0) {
        return AEGIS_ERR_INVALID_REQUEST;
    }

    /* The vocabulary is enforced here rather than at the wire layer so no
     * writer can route around it — and ahead of the type dispatch so that
     * includes working memory, which returns before the persisted path and
     * would otherwise accept an undeclared predicate on a server that
     * configured a registry. Deliberately *not* in validate_common, which
     * `update` also calls: a registry edited between runs would then make an
     * old record's existing fact block an unrelated edit to its tags. A fact is
     * immutable, so checking it once at insert is checking it exactly when it
     * can change. The replica path deliberately does not check: it applies what
     * the primary already accepted, and re-judging would desync the two. */
    if (in->fact.kind != FACT_NONE && db->predicates) {
        char why[256];
        if (predicate_registry_check(db->predicates, in->fact.predicate,
                                     in->fact.kind, why, sizeof why) != 0) {
            LOG_DEBUG("insert refused: %s", why);
            return AEGIS_ERR_INVALID_REQUEST;
        }
    }

    if (in->type == MEM_WORKING) {
        st = require_phase(db, 4);
        if (st != AEGIS_OK) {
            return st;
        }
        if (!session_id) {
            return AEGIS_ERR_INVALID_REQUEST;
        }
        uint64_t now = db_now_ms();
        uint64_t wid = 0;
        if (working_store_add(db->working, session_id, in, now, ttl_ms, &wid) !=
            0) {
            return AEGIS_ERR_INTERNAL;
        }
        MemoryRecord *got =
            working_store_get(db->working, session_id, wid, now);
        if (!got) {
            return AEGIS_ERR_INTERNAL;
        }
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
    if (st != AEGIS_OK) {
        return st;
    }

    /* Backpressure: refuse a persisted insert once index RAM reaches the cap, so
     * a growing dataset returns MEMORY_LIMIT instead of getting OOM-killed. The
     * sampled value (maintenance thread) is read lock-free; deletes/updates still
     * proceed so memory can be freed. Working memory (above) is a bounded ring
     * and is exempt. */
    if (db->config.max_index_bytes &&
        atomic_load_explicit(&db->index_bytes, memory_order_relaxed) >=
            db->config.max_index_bytes) {
        return AEGIS_ERR_MEMORY_LIMIT;
    }

    MemoryRecord *rec = record_clone(in);
    if (!rec) {
        return AEGIS_ERR_INTERNAL;
    }
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
        for (size_t i = 0; i < rec->tag_count; i++) {
            tag_index_add(db->tags, rec->tags[i], rec->id);
        }
        lexical_index_add(db->lex, rec->id, rec->data, rec->data_len);
        /* An `insert` carries no relationships today — they arrive only via
         * `relate` — so this indexes nothing. It is here so the insert path
         * stays uniform with db_replica_apply, which *does* receive records
         * with edges, rather than encoding "inserts have no edges" as an
         * invariant a future promote/batch path could quietly break. */
        for (size_t i = 0; i < rec->rel_count; i++) {
            edge_index_add(db->edges, rec->id, rec->relationships[i].to_id,
                           rec->relationships[i].kind);
        }
        db_fact_index_apply(db, rec, 1);
        usage_index_track(db->usage, rec->id);
        if (rec->embedding_dim && rec->vec_count) {
            semantic_index_add(db->sem, rec->id, rec->embedding, rec->vec_count,
                               rec->embedding_dim);
        }
    }
    pthread_rwlock_unlock(&db->index_lock);
    if (st == AEGIS_OK && durably_flush(db) != 0) {
        st = AEGIS_ERR_INTERNAL; /* not durable: do not acknowledge the write */
    }

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
    if (st != AEGIS_OK) {
        return st;
    }
    if (out->deleted || record_expired(out, db_now_ms())) {
        record_free(out);
        return AEGIS_ERR_NOT_FOUND;
    }
    if (agent_filter &&
        (!out->agent_id || strcmp(out->agent_id, agent_filter) != 0)) {
        record_free(out);
        return AEGIS_ERR_NOT_FOUND;
    }
    return AEGIS_OK;
}

aegis_status_t qe_update(AegisDB *db, uint64_t id, const UpdatePatch *patch,
                         const char *ns, MemoryRecord *out) {
    aegis_status_t st = require_phase(db, 2);
    if (st != AEGIS_OK) {
        return st;
    }

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
    /* A fact can be *adopted* onto a record that asserts none, but never
     * changed: the immutability rule is about altering a claim, and there is
     * none to alter when the field is empty. Refused rather than ignored, so a
     * caller cannot quietly get the opposite of what it asked for — and
     * refused here, before anything is detached from the record, so the error
     * path has nothing to leak. `update` rejects the field at the wire layer
     * regardless; this exists for consolidation, which would otherwise destroy
     * the assertion of the record it absorbs. */
    if (patch->has_fact && patch->fact && cur.fact.kind != FACT_NONE) {
        record_free(&cur);
        pthread_rwlock_unlock(&db->index_lock);
        return AEGIS_ERR_INVALID_REQUEST;
    }

    /* old tags for index diff */
    /* Detach (rather than free) the superseded payload: the lexical index is
     * keyed on the exact bytes that were indexed, so unindexing the old version
     * needs them — and, like the tag swing below, that must not happen until the
     * append has committed. */
    void *old_data = NULL;
    size_t old_data_len = 0;
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
        old_data = cur.data;
        old_data_len = cur.data_len;
        cur.data = nd;
        cur.data_len = patch->data_len;
    }
    if (patch->has_importance) {
        cur.importance = patch->importance;
    }
    if (patch->has_confidence) {
        cur.confidence = patch->confidence;
    }

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

    int adopted_fact = 0;
    if (patch->has_fact && patch->fact) {
        if (record_set_fact(&cur, patch->fact->kind, patch->fact->subject,
                            patch->fact->predicate, patch->fact->object_id,
                            patch->fact->object_str) != 0) {
            cur.tags = old_tags; /* reattach so record_free owns them again */
            cur.tag_count = old_tag_count;
            record_free(&cur);
            pthread_rwlock_unlock(&db->index_lock);
            return AEGIS_ERR_INTERNAL;
        }
        adopted_fact = 1;
    }

    cur.updated = db_now_ms();
    st = validate_common(db, &cur);
    if (st == AEGIS_OK) {
        st = append_and_hash(db, &cur);
    }
    if (st == AEGIS_OK && patch->has_tags) {
        /* Committed: now it's safe to swing the tag index from old to new. */
        for (size_t i = 0; i < old_tag_count; i++) {
            tag_index_remove(db->tags, old_tags[i], cur.id);
        }
        for (size_t i = 0; i < cur.tag_count; i++) {
            tag_index_add(db->tags, cur.tags[i], cur.id);
        }
    }
    if (st == AEGIS_OK && adopted_fact) {
        /* The only fact-index work an update ever does. It is an add, never a
         * swing: the record had no fact a moment ago, so there is nothing to
         * unindex. */
        db_fact_index_apply(db, &cur, 1);
    }
    /* No edge-index work here on purpose, and no fact *change*. An update patch
     * reaches tags, the payload, and a fact only where none existed — never
     * `relationships` (relate/delete own those), and never a rewrite of an
     * existing claim, which is a supersession rather than an edit and leaves
     * the auditable chain 2.1/2.2 already provide. */
    if (st == AEGIS_OK && patch->has_data) {
        /* Same discipline for the payload text: unindex the version that just
         * stopped being current, then index the one that replaced it. */
        lexical_index_remove(db->lex, cur.id, old_data, old_data_len);
        lexical_index_add(db->lex, cur.id, cur.data, cur.data_len);
    }
    pthread_rwlock_unlock(&db->index_lock);
    if (st == AEGIS_OK && durably_flush(db) != 0) {
        st = AEGIS_ERR_INTERNAL; /* not durable: do not acknowledge the write */
    }

    for (size_t i = 0; i < old_tag_count; i++) {
        free(old_tags[i]);
    }
    free(old_tags);
    free(old_data);

    if (st != AEGIS_OK) {
        record_free(&cur);
        return st;
    }
    *out = cur; /* move ownership */
    return AEGIS_OK;
}

aegis_status_t qe_delete(AegisDB *db, uint64_t id, const char *ns) {
    aegis_status_t st = require_phase(db, 1);
    if (st != AEGIS_OK) {
        return st;
    }

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
    for (size_t i = 0; i < cur.tag_count; i++) {
        tag_index_remove(db->tags, cur.tags[i], cur.id);
    }
    lexical_index_remove(db->lex, cur.id, cur.data, cur.data_len);
    /* Outgoing: the record in hand lists its own edges. */
    for (size_t i = 0; i < cur.rel_count; i++) {
        edge_index_remove(db->edges, cur.id, cur.relationships[i].to_id,
                          cur.relationships[i].kind);
    }
    /* Incoming: O(indegree) precisely *because* this index exists — without it
     * finding who points here would be a corpus scan. The peers' records still
     * list the tombstoned id and are deliberately not rewritten (a tombstone
     * never touches other records); a forward walk already skips a target whose
     * hash entry is absent or deleted. So the invariant is: the edge index never
     * reports an edge whose endpoint is not live, though a record's own
     * relationships array may still name one. */
    /* Before the reverse edges go: a conclusion drawn from this record cites
     * it as a premise, and after edge_index_remove_target there is nothing
     * left to walk. Capturing here — under the write lock this function
     * already holds — is what makes retraction possible without either a
     * corpus scan or an unbounded cascade inside a client's delete. */
    db_retract_enqueue(db, cur.id);
    edge_index_remove_target(db->edges, cur.id);
    db_fact_index_apply(db, &cur, 0);
    usage_index_untrack(db->usage, cur.id);
    if (cur.embedding_dim) {
        semantic_index_remove(db->sem, cur.id);
    }
    time_index_remove(db->time, cur.created, cur.id);

    cur.deleted = 1;
    cur.updated = db_now_ms();
    st = append_and_hash(db, &cur); /* tombstone version; hash marks deleted */
    pthread_rwlock_unlock(&db->index_lock);
    if (st == AEGIS_OK && durably_flush(db) != 0) {
        st = AEGIS_ERR_INTERNAL; /* not durable: do not acknowledge the write */
    }

    record_free(&cur);
    return st;
}

aegis_status_t qe_promote(AegisDB *db, const char *session_id,
                          uint64_t working_id, MemoryType to_type,
                          const char *ns, MemoryRecord *out) {
    aegis_status_t st = require_phase(db, 4);
    if (st != AEGIS_OK) {
        return st;
    }
    if (!session_id) {
        return AEGIS_ERR_INVALID_REQUEST;
    }
    if (to_type != MEM_EPISODIC && to_type != MEM_SEMANTIC) {
        return AEGIS_ERR_INVALID_REQUEST;
    }

    MemoryRecord w;
    if (working_store_take(db->working, session_id, working_id, db_now_ms(), ns,
                           &w) != 0) {
        return AEGIS_ERR_NOT_FOUND;
    }

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
    if (e->to_id != to_id) {
        return 0;
    }
    if (!e->kind || !kind) {
        return e->kind == kind; /* both NULL == equal */
    }
    return strcmp(e->kind, kind) == 0;
}

aegis_status_t qe_relate(AegisDB *db, uint64_t from_id, uint64_t to_id,
                         const char *kind, const char *ns) {
    aegis_status_t st = require_phase(db, 4);
    if (st != AEGIS_OK) {
        return st;
    }
    /* A self-edge carries no graph information and would still consume a slot. */
    if (from_id == to_id) {
        return AEGIS_ERR_INVALID_REQUEST;
    }
    /* Bound the kind so the reverse index can always intern it. Beyond this the
     * edge would still be indexed, but with its label unknown, which turns a
     * filtered reverse walk into a candidate set — a silent loss of precision
     * that no caller asked for. Rejecting is the honest answer. */
    if (kind && strlen(kind) > MAX_REL_KIND_LEN) {
        return AEGIS_ERR_INVALID_REQUEST;
    }

    pthread_rwlock_wrlock(&db->index_lock);
    MemoryRecord from;
    st = load_record(db, from_id, &from);
    if (st != AEGIS_OK || from.deleted || ns_denies(ns, &from)) {
        if (st == AEGIS_OK) {
            record_free(&from);
        }
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
            if (tst == AEGIS_OK) {
                record_free(&to_rec);
            }
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
    if (st == AEGIS_OK) {
        /* The primary maintenance site for the reverse index: `relate` is the
         * only operation that creates an edge. Indexed after the append commits,
         * so a failed write leaves no phantom edge behind. Both endpoints were
         * verified live above, which is what upholds the liveness invariant
         * documented in qe_delete. */
        edge_index_add(db->edges, from_id, to_id, kind);
    }
    pthread_rwlock_unlock(&db->index_lock);
    if (st == AEGIS_OK && durably_flush(db) != 0) {
        st = AEGIS_ERR_INTERNAL; /* not durable: do not acknowledge the write */
    }
    record_free(&from);
    return st;
}

/* Does an edge of kind `kind` pass the filter? An empty filter follows
 * everything (the pre-5.1 behaviour). A NULL edge kind passes only the empty
 * filter: once a caller names the kinds it wants, an unkinded edge is not one
 * of them. */
static int kind_wanted(const char *const *kinds, size_t n, const char *kind) {
    if (n == 0) {
        return 1;
    }
    if (!kind) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (kinds[i] && strcmp(kinds[i], kind) == 0) {
            return 1;
        }
    }
    return 0;
}

/* One BFS frontier entry: an id to visit, plus the edge that reached it.
 *
 * `via_kind` is **owned**, copied at enqueue time. It would be tempting to
 * borrow it — a forward edge's kind lives in the parent record, which the walk
 * holds in `acc` for the duration — but a *reverse* edge's kind is an interned
 * string in the shared EdgeIndex, and the frontier outlives the index_lock
 * acquisition that produced it. A replica re-bootstrapping in that window
 * (follower_reset takes index_lock for write and frees the whole index) would
 * leave the pointer dangling. Uniform ownership is the only version of this
 * whose correctness does not depend on which branch created the entry. */
typedef struct {
    uint64_t id;
    uint64_t via_id;
    char *via_kind;
    int incoming; /* 1 if this id was reached by walking an edge backwards */
    int kind_uncertain; /* reverse only: the edge's kind could not be interned,
                         * so via_kind is unknown rather than absent */
} TravEdge;

/* One accumulated hit: the record plus how the walk reached it. */
typedef struct {
    MemoryRecord rec;
    int depth;
    uint64_t via_id;
    char *via_kind; /* owned */
    int via_incoming;
    int via_kind_uncertain;
} TravNode;

/* Release a frontier and every label it still owns. */
static void travedges_free(TravEdge *e, size_t n) {
    if (!e) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(e[i].via_kind);
    }
    free(e);
}

/* Enqueue one neighbour, copying the reaching edge's kind. A failed copy costs
 * the label, not the hop. Returns 0, or -1 if the frontier could not grow. */
static int travedge_push(TravEdge **next, size_t *n, size_t *cap, uint64_t id,
                         uint64_t via_id, const char *via_kind, int incoming,
                         int kind_uncertain) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        TravEdge *tmp = realloc(*next, nc * sizeof(**next));
        if (!tmp) {
            return -1;
        }
        *next = tmp;
        *cap = nc;
    }
    (*next)[*n].id = id;
    (*next)[*n].via_id = via_id;
    (*next)[*n].via_kind = via_kind ? strdup(via_kind) : NULL;
    (*next)[*n].incoming = incoming;
    (*next)[*n].kind_uncertain = kind_uncertain;
    (*n)++;
    return 0;
}

/* Visited set for one traversal: open-addressed, linear probing, fixed at
 * TRAVERSE_SEEN_SLOTS so it never grows (the node cap keeps its load factor at
 * or under 0.5). Record ids start at 1, so 0 marks an empty slot.
 *
 * This replaced a linear scan of a growing array. That was O(frontier x seen)
 * *while holding index_lock*, which only became a real hazard when reverse
 * traversal exposed unbounded indegree: writers need the write lock, so a
 * quadratic scan over a hub's sources starved every one of them. Returns 1 if
 * `id` was newly inserted, 0 if it was already present. */
static int seen_insert(uint64_t *tbl, uint64_t id) {
    size_t mask = TRAVERSE_SEEN_SLOTS - 1;
    size_t i = (size_t)mix64(id) & mask;
    for (;;) {
        if (tbl[i] == 0) {
            tbl[i] = id;
            return 1;
        }
        if (tbl[i] == id) {
            return 0;
        }
        i = (i + 1) & mask;
    }
}

void traverse_hops_free(TraverseHop *hops, size_t n) {
    if (!hops) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(hops[i].via_kind);
    }
    free(hops);
}

aegis_status_t qe_traverse(AegisDB *db, uint64_t start_id, int depth,
                           const char *agent_filter, MemoryRecord **out,
                           size_t *out_n) {
    TraverseParams p;
    memset(&p, 0, sizeof(p));
    p.start_id = start_id;
    p.depth = depth;
    p.agent_filter = agent_filter;
    p.direction = TRAVERSE_OUT;
    return qe_traverse_ex(db, &p, out, NULL, out_n, NULL);
}

aegis_status_t qe_traverse_ex(AegisDB *db, const TraverseParams *p,
                              MemoryRecord **out, TraverseHop **out_hops,
                              size_t *out_n, int *out_capped) {
    if (out_capped) {
        *out_capped = 0;
    }
    aegis_status_t st = require_phase(db, 4);
    if (st != AEGIS_OK) {
        return st;
    }
    int depth = p->depth < 0 ? 0 : p->depth;
    const char *agent_filter = p->agent_filter;
    int walk_out = p->direction != TRAVERSE_IN;
    int walk_in = p->direction != TRAVERSE_OUT;
    /* Walking backwards is the one direction that needs an index: a record
     * lists the edges it points along, not the ones pointing at it. */
    if (walk_in && !db->edges) {
        return AEGIS_ERR_NOT_READY;
    }
    uint64_t now =
        db_now_ms(); /* for expiry, sampled once for the whole walk */

    /* BFS over relationship edges */
    uint64_t *seen = calloc(TRAVERSE_SEEN_SLOTS, sizeof(uint64_t));
    size_t seen_n = 0;
    int capped = 0;
    TravEdge *frontier = malloc(sizeof(TravEdge));
    size_t front_n = 0;
    if (!frontier || !seen) {
        free(frontier);
        free(seen);
        return AEGIS_ERR_INTERNAL;
    }
    frontier[front_n].id = p->start_id;
    frontier[front_n].via_id = 0;
    frontier[front_n].via_kind = NULL;
    frontier[front_n].incoming = 0;
    frontier[front_n].kind_uncertain = 0;
    front_n++;

    TravNode *acc = NULL;
    size_t acc_n = 0;
    size_t acc_cap = 0;

    /* On any allocation failure we stop growing and return what we have so far
     * (like the malloc(offs) guard below), rather than dereferencing a failed
     * realloc's NULL or leaking the old buffer it left untouched. */
    int oom = 0;
    for (int level = 0; level <= depth && front_n > 0 && !oom && !capped;
         level++) {
        /* Resolve this level's not-yet-seen ids to log offsets under the index
         * lock, then read+decode them off it (disk I/O under log_lock only). */
        uint64_t *offs = malloc(front_n * sizeof(uint64_t));
        /* Parallel to `offs`: the edge that reached each id, carried across the
         * lock handoff so the read phase can attribute a decoded record without
         * having to resolve it again. */
        TravEdge *vias = malloc(front_n * sizeof(TravEdge));
        if (!offs || !vias) {
            free(offs);
            free(vias);
            break; /* frontier freed after the loop; return what we have */
        }
        size_t off_n = 0;

        pthread_rwlock_rdlock(&db->index_lock);
        for (size_t i = 0; i < front_n; i++) {
            uint64_t id = frontier[i].id;
            /* Stop before exceeding the node cap rather than after: the result
             * is a prefix of the graph, and the caller is told so. Remaining
             * labels are released by the loop after this one. */
            if (seen_n >= TRAVERSE_MAX_NODES) {
                capped = 1;
                break;
            }
            if (!seen_insert(seen, id)) {
                free(frontier[i].via_kind);
                frontier[i].via_kind = NULL;
                continue;
            }
            seen_n++;
            const HashEntry *e = hash_index_get(db->hash, id);
            if (!e) {
                free(frontier[i].via_kind);
                frontier[i].via_kind = NULL;
                continue;
            }
            vias[off_n] = frontier[i];   /* moves ownership of via_kind */
            frontier[i].via_kind = NULL; /* ...so only `vias` owns it now */
            offs[off_n++] = e->offset;
        }
        pthread_rwlock_rdlock(&db->log_lock);
        pthread_rwlock_unlock(&db->index_lock);
        /* Release whatever labels are still owned here. Every entry consumed
         * above nulled its own pointer — freed or moved — so this is safe from
         * index 0 no matter where the loop stopped. Keying it on the break index
         * instead would be one off-by-one away from a double free, and the oom
         * path has no test that would catch that. */
        for (size_t i = 0; i < front_n; i++) {
            free(frontier[i].via_kind);
        }
        free(frontier);
        frontier = NULL;

        TravEdge *next = NULL;
        size_t next_n = 0;
        size_t next_cap = 0;
        size_t level_start = acc_n; /* first hit accumulated at this level */
        for (size_t i = 0; i < off_n && !oom; i++) {
            uint8_t *buf = NULL;
            size_t len = 0;
            if (log_read(&db->log, offs[i], &buf, &len) != 0) {
                continue;
            }
            MemoryRecord r;
            int dec = record_decode(buf, len, &r);
            free(buf);
            if (dec != 0) {
                continue;
            }
            if (r.deleted || record_expired(&r, now) ||
                (agent_filter &&
                 (!r.agent_id || strcmp(r.agent_id, agent_filter) != 0))) {
                /* a filtered/expired node is skipped entirely, edges and all */
                record_free(&r);
                continue;
            }
            /* collect */
            if (acc_n == acc_cap) {
                size_t nc = acc_cap ? acc_cap * 2 : 8;
                TravNode *tmp = realloc(acc, nc * sizeof(TravNode));
                if (!tmp) {
                    record_free(&r);
                    oom = 1;
                    break;
                }
                acc = tmp;
                acc_cap = nc;
            }
            acc[acc_n].rec = r; /* keep; do not free */
            acc[acc_n].depth = level;
            acc[acc_n].via_id = vias[i].via_id;
            acc[acc_n].via_kind = vias[i].via_kind; /* move */
            vias[i].via_kind = NULL;
            acc[acc_n].via_incoming = vias[i].incoming;
            acc[acc_n].via_kind_uncertain = vias[i].kind_uncertain;
            acc_n++;
            /* Enqueue outgoing neighbours. `r` still aliases the record just
             * moved into acc, so its relationship strings outlive this level. */
            for (size_t k = 0; walk_out && k < r.rel_count; k++) {
                if (!kind_wanted(p->kinds, p->kind_count,
                                 r.relationships[k].kind)) {
                    continue;
                }
                if (travedge_push(&next, &next_n, &next_cap,
                                  r.relationships[k].to_id, r.id,
                                  r.relationships[k].kind, 0, 0) != 0) {
                    oom = 1;
                    break;
                }
            }
        }
        pthread_rwlock_unlock(&db->log_lock);

        /* Reverse expansion. Incoming edges live in the edge index, so they are
         * gathered under index_lock — a separate phase from the forward
         * expansion above, which reads them straight out of the record under
         * log_lock. Both feed the same frontier. log_lock is dropped first
         * because the lock order is index -> log; re-taking index while holding
         * log would invert it. Only hits that survived this level's filters are
         * expanded, matching the forward rule that a filtered node is skipped
         * entirely, edges and all. */
        if (walk_in && !oom) {
            pthread_rwlock_rdlock(&db->index_lock);
            for (size_t a = level_start; a < acc_n && !oom; a++) {
                EdgeSource *srcs = NULL;
                size_t sn = 0;
                if (edge_index_sources(db->edges, acc[a].rec.id, p->kinds,
                                       p->kind_count, &srcs, &sn) != 0) {
                    oom = 1;
                    break;
                }
                for (size_t j = 0; j < sn; j++) {
                    if (travedge_push(&next, &next_n, &next_cap,
                                      srcs[j].from_id, acc[a].rec.id,
                                      srcs[j].kind, 1,
                                      srcs[j].kind_unknown) != 0) {
                        oom = 1;
                        break;
                    }
                }
                free(srcs);
            }
            pthread_rwlock_unlock(&db->index_lock);
        }

        free(offs);
        travedges_free(vias, off_n); /* frees only labels not moved into acc */
        frontier = next;
        front_n = next_n;
        (void)next_cap;
    }
    travedges_free(frontier, front_n);
    free(seen);
    if (out_capped) {
        *out_capped = capped;
    }

    MemoryRecord *res = malloc((acc_n ? acc_n : 1) * sizeof(MemoryRecord));
    TraverseHop *hops = NULL;
    if (out_hops) {
        hops = malloc((acc_n ? acc_n : 1) * sizeof(TraverseHop));
    }
    if (!res || (out_hops && !hops)) {
        free(res);
        free(hops);
        for (size_t i = 0; i < acc_n; i++) {
            record_free(&acc[i].rec);
            free(acc[i].via_kind);
        }
        free(acc);
        return AEGIS_ERR_INTERNAL;
    }
    for (size_t i = 0; i < acc_n; i++) {
        res[i] = acc[i].rec;
        if (hops) {
            hops[i].depth = acc[i].depth;
            hops[i].via_id = acc[i].via_id;
            hops[i].via_incoming = acc[i].via_incoming;
            hops[i].via_kind_uncertain = acc[i].via_kind_uncertain;
            hops[i].via_kind = acc[i].via_kind; /* move ownership */
        } else {
            free(acc[i].via_kind); /* attribution not wanted; drop the label */
        }
    }
    free(acc);
    *out = res;
    if (out_hops) {
        *out_hops = hops;
    }
    *out_n = acc_n;
    return AEGIS_OK;
}
