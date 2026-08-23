/* Query engine — maintenance: export/purge, temporal history, sweep,
 * consolidate, forget (split from query_engine.c). */
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

static int cmp_u64_asc(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return AEGIS_CMP3(x, y);
}

/* Snapshot every live record id (optionally only those > after_id) under the
 * index lock. Returns a malloc'd, unsorted array; caller frees. */
/* Collect the ids of the live hash entries (used, not deleted) for which
 * keep(entry, ctx) is true, under the index read lock. Returns a malloc'd array
 * of *out_n ids (caller frees; NULL when none matched). On a mid-scan allocation
 * failure it returns what it gathered so far and, if out_oom is non-NULL, sets
 * *out_oom = 1 — callers that must not act on a partial set check it and bail;
 * best-effort callers pass NULL. */
static uint64_t *collect_ids(AegisDB *db,
                             int (*keep)(const HashEntry *, void *), void *ctx,
                             size_t *out_n, int *out_oom) {
    if (out_oom) {
        *out_oom = 0;
    }
    pthread_rwlock_rdlock(&db->index_lock);
    const HashIndex *h = db->hash;
    uint64_t *ids = NULL;
    size_t n = 0;
    size_t cap = 0;
    for (size_t i = 0; i < h->cap; i++) {
        const HashEntry *e = &h->buckets[i];
        if (!e->used || e->deleted || !keep(e, ctx)) {
            continue;
        }
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 64;
            uint64_t *g = realloc(ids, nc * sizeof(uint64_t));
            if (!g) {
                if (out_oom) {
                    *out_oom = 1;
                }
                break;
            }
            ids = g;
            cap = nc;
        }
        ids[n++] = e->id;
    }
    pthread_rwlock_unlock(&db->index_lock);
    *out_n = n;
    return ids;
}

/* collect_ids predicates: ctx points at the comparison value (or is unused). */
static int keep_after_id(const HashEntry *e, void *ctx) {
    return e->id > *(const uint64_t *)ctx;
}
static int keep_expired(const HashEntry *e, void *ctx) {
    uint64_t now = *(const uint64_t *)ctx;
    return e->expires_at != 0 && now >= e->expires_at;
}
static int keep_semantic(const HashEntry *e, void *ctx) {
    (void)ctx;
    return e->type == MEM_SEMANTIC;
}
static int keep_type(const HashEntry *e, void *ctx) {
    return e->type == *(const MemoryType *)ctx;
}

static uint64_t *snapshot_live_ids(AegisDB *db, uint64_t after_id,
                                   size_t *out_n) {
    return collect_ids(db, keep_after_id, &after_id, out_n, NULL);
}

/* Export (ROADMAP 3.2): the subject's records, id-ordered, paginated by
 * after_id. Only records owned by `ns` are returned (qe_get enforces the
 * namespace), so a tenant exports exactly its own data. */
aegis_status_t qe_export(AegisDB *db, const char *ns, uint64_t after_id,
                         size_t limit, MemoryRecord **out_records,
                         size_t *out_n, int *out_has_more) {
    aegis_status_t st = require_phase(db, 1);
    if (st != AEGIS_OK) {
        return st;
    }
    *out_records = NULL;
    *out_n = 0;
    if (out_has_more) {
        *out_has_more = 0;
    }
    if (limit == 0) {
        limit = 100;
    }

    size_t n = 0;
    uint64_t *ids = snapshot_live_ids(db, after_id, &n);
    if (n) {
        qsort(ids, n, sizeof(uint64_t), cmp_u64_asc);
    }

    MemoryRecord *out = malloc(limit * sizeof(MemoryRecord));
    if (!out) {
        free(ids);
        return AEGIS_ERR_INTERNAL;
    }
    size_t got = 0;
    int more = 0;
    for (size_t i = 0; i < n; i++) {
        if (got == limit) {
            more = 1; /* at least one candidate remains for the next page */
            break;
        }
        MemoryRecord r;
        if (qe_get(db, ids[i], ns, &r) != AEGIS_OK) {
            continue; /* not this tenant's */
        }
        out[got++] = r; /* move ownership */
    }
    free(ids);
    *out_records = out;
    *out_n = got;
    if (out_has_more) {
        *out_has_more = more;
    }
    return AEGIS_OK;
}

/* Right-to-be-forgotten (ROADMAP 3.2): tombstone every record owned by `ns`.
 * `ns` must be a concrete namespace — a global purge is refused. The caller runs
 * compaction afterward so the payloads actually leave the on-disk log. dry_run
 * counts the subject's records without deleting. */
aegis_status_t qe_purge_namespace(AegisDB *db, const char *ns, int dry_run,
                                  size_t *out_count) {
    aegis_status_t st = require_phase(db, 1);
    if (st != AEGIS_OK) {
        return st;
    }
    if (!ns || !*ns) {
        return AEGIS_ERR_INVALID_REQUEST; /* never purge everything */
    }
    *out_count = 0;

    size_t n = 0;
    uint64_t *ids = snapshot_live_ids(db, 0, &n);
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (dry_run) {
            MemoryRecord r;
            if (qe_get(db, ids[i], ns, &r) == AEGIS_OK) {
                count++;
                record_free(&r);
            }
        } else if (qe_delete(db, ids[i], ns) == AEGIS_OK) {
            count++; /* qe_delete enforces ns: only this tenant's records go */
        }
    }
    free(ids);
    *out_count = count;
    if (!dry_run) {
        atomic_fetch_add_explicit(&db->metrics.memories_purged, count,
                                  memory_order_relaxed);
    }
    return AEGIS_OK;
}

/* --- Temporal / point-in-time reads (ROADMAP 3.1) --------------------------
 * The append-only log holds every version of every record (updates append a new
 * version, deletes append a tombstone), so history and "as of time T" reads are
 * a log scan. These are diagnostic/audit ops — O(log size) — run under log_lock
 * so a concurrent compaction swap can't move the file underneath the scan. */

typedef struct {
    uint64_t id;
    MemoryRecord *arr;
    size_t n, cap;
    int err;
} HistCtx;

static int history_cb(uint64_t offset, const uint8_t *payload, size_t len,
                      void *ctx) {
    (void)offset;
    HistCtx *h = ctx;
    MemoryRecord r;
    if (record_decode(payload, len, &r) != 0) {
        return 0;
    }
    if (r.id != h->id) {
        record_free(&r);
        return 0;
    }
    if (h->n == h->cap) {
        size_t nc = h->cap ? h->cap * 2 : 8;
        MemoryRecord *g = realloc(h->arr, nc * sizeof(MemoryRecord));
        if (!g) {
            record_free(&r);
            h->err = 1;
            return 1; /* abort scan */
        }
        h->arr = g;
        h->cap = nc;
    }
    h->arr[h->n++] = r; /* keep in causal (log/append) order; ownership moved */
    return 0;
}

aegis_status_t qe_history(AegisDB *db, uint64_t id, const char *ns,
                          MemoryRecord **out_versions, size_t *out_n) {
    aegis_status_t st = require_phase(db, 1);
    if (st != AEGIS_OK) {
        return st;
    }
    *out_versions = NULL;
    *out_n = 0;

    HistCtx h = {id, NULL, 0, 0, 0};
    LogScanResult res;
    /* Snapshot the scan end under index_lock (appends bump log.size under it),
     * while holding log_lock to pin the log against a compaction swap; release
     * index_lock before the scan so writers aren't blocked for its duration. */
    pthread_rwlock_rdlock(&db->index_lock);
    pthread_rwlock_rdlock(&db->log_lock);
    uint64_t end = (uint64_t)db->log.size;
    pthread_rwlock_unlock(&db->index_lock);
    int rc = log_scan(&db->log, 0, end, history_cb, &h, &res);
    pthread_rwlock_unlock(&db->log_lock);
    if (rc != 0 || h.err) {
        for (size_t i = 0; i < h.n; i++) {
            record_free(&h.arr[i]);
        }
        free(h.arr);
        return AEGIS_ERR_INTERNAL;
    }
    if (h.n == 0) {
        return AEGIS_ERR_NOT_FOUND; /* id never existed */
    }
    /* All versions of an id share its agent_id, so one namespace check suffices;
     * a cross-tenant id reads as NOT_FOUND (no existence leak). */
    if (ns_denies(ns, &h.arr[h.n - 1])) {
        for (size_t i = 0; i < h.n; i++) {
            record_free(&h.arr[i]);
        }
        free(h.arr);
        return AEGIS_ERR_NOT_FOUND;
    }
    *out_versions = h.arr;
    *out_n = h.n;
    return AEGIS_OK;
}

typedef struct {
    uint64_t id;
    uint64_t as_of;
    int have;
    MemoryRecord best;
} AsOfCtx;

static int as_of_cb(uint64_t offset, const uint8_t *payload, size_t len,
                    void *ctx) {
    (void)offset;
    AsOfCtx *a = ctx;
    MemoryRecord r;
    if (record_decode(payload, len, &r) != 0) {
        return 0;
    }
    if (r.id != a->id || r.updated > a->as_of) {
        record_free(&r);
        return 0;
    }
    /* Scan is in append (causal) order, so the last version with updated <= as_of
     * is the one live at that time. */
    if (a->have) {
        record_free(&a->best);
    }
    a->best = r;
    a->have = 1;
    return 0;
}

aegis_status_t qe_get_as_of(AegisDB *db, uint64_t id, const char *ns,
                            uint64_t as_of, MemoryRecord *out) {
    aegis_status_t st = require_phase(db, 1);
    if (st != AEGIS_OK) {
        return st;
    }

    AsOfCtx a = {id, as_of, 0, {0}};
    LogScanResult res;
    /* See qe_get_history: snapshot the end under index_lock, scan under log_lock. */
    pthread_rwlock_rdlock(&db->index_lock);
    pthread_rwlock_rdlock(&db->log_lock);
    uint64_t end = (uint64_t)db->log.size;
    pthread_rwlock_unlock(&db->index_lock);
    int rc = log_scan(&db->log, 0, end, as_of_cb, &a, &res);
    pthread_rwlock_unlock(&db->log_lock);
    if (rc != 0) {
        if (a.have) {
            record_free(&a.best);
        }
        return AEGIS_ERR_INTERNAL;
    }
    if (!a.have) {
        return AEGIS_ERR_NOT_FOUND; /* did not exist at as_of */
    }
    /* A tombstone or another tenant's record reads as NOT_FOUND: as of that
     * time, this caller "knew" nothing here. */
    if (a.best.deleted || ns_denies(ns, &a.best)) {
        record_free(&a.best);
        return AEGIS_ERR_NOT_FOUND;
    }
    *out = a.best; /* move ownership */
    return AEGIS_OK;
}

size_t qe_sweep_expired(AegisDB *db, uint64_t now) {
    /* Collect expired live ids from an in-memory hash scan (no record reads —
     * expires_at lives in the HashEntry), then tombstone each so compaction can
     * reclaim the log. Snapshot-then-delete: qe_delete takes the write lock and
     * re-validates, so a racing change just skips that id. Best-effort on OOM. */
    size_t n = 0;
    uint64_t *ids = collect_ids(db, keep_expired, &now, &n, NULL);

    size_t swept = 0;
    for (size_t i = 0; i < n; i++) {
        if (qe_delete(db, ids[i], NULL) == AEGIS_OK) {
            swept++;
        }
    }
    free(ids);
    return swept;
}

#define CONSOLIDATE_CAP                                                        \
    256 /* max cluster members merged per pass (bounds work) */

/* Merge one near-duplicate cluster (the records in `recs`, all semantic, same
 * namespace, mutually >= min_similarity): keep the most-recently-updated as the
 * survivor, union the losers' tags into it, take the max importance/confidence,
 * migrate the losers' relationships onto it, and tombstone the losers. Returns
 * the number of records merged away (losers deleted). */
/* Do these two records assert the same triple? A merge only preserves what a
 * conclusion rests on when the survivor says the same thing the loser did. */
static int facts_equal(const Fact *a, const Fact *b) {
    if (a->kind != b->kind || a->subject != b->subject) {
        return 0;
    }
    if (!a->predicate || !b->predicate ||
        strcmp(a->predicate, b->predicate) != 0) {
        return 0;
    }
    if (a->kind == FACT_OBJ_ID) {
        return a->object_id == b->object_id;
    }
    return a->object_str && b->object_str &&
           strcmp(a->object_str, b->object_str) == 0;
}

/* Give every conclusion that cites `old_id` an edge to its heir, so a later
 * delete of the heir still finds them. Best-effort: what this misses, recovery
 * reconciles. */
static void repoint_dependents(AegisDB *db, uint64_t old_id, uint64_t heir,
                               const char *ns) {
    static const char *const KIND[] = {"derived_from"};
    EdgeSource *src = NULL;
    size_t n = 0;
    pthread_rwlock_rdlock(&db->index_lock);
    int rv = edge_index_sources(db->edges, old_id, KIND, 1, &src, &n);
    pthread_rwlock_unlock(&db->index_lock);
    if (rv != 0) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        (void)qe_relate(db, src[i].from_id, heir, "derived_from", ns);
    }
    free(src);
}

static size_t merge_cluster(AegisDB *db, MemoryRecord *recs, size_t n,
                            const char *ns) {
    /* survivor = latest updated (tie -> greatest id) */
    size_t sv = 0;
    for (size_t i = 1; i < n; i++) {
        if (recs[i].updated > recs[sv].updated ||
            (recs[i].updated == recs[sv].updated && recs[i].id > recs[sv].id)) {
            sv = i;
        }
    }
    uint64_t survivor = recs[sv].id;

    /* What the survivor will assert once this is done: its own fact, or the
     * first one in the cluster if it has none.
     *
     * A merge must not destroy an assertion. UpdatePatch carried tags,
     * importance and confidence but never the fact, so absorbing a
     * fact-bearing record into one without a fact silently dropped what it
     * asserted — stats.facts fell and no pattern could find it again. That
     * predates 5.3 and was invisible until truth maintenance started asking
     * whether a merged record still supports anything. Adoption only, never a
     * rewrite: writing a fact where none existed overwrites no claim, which is
     * what makes it compatible with facts being immutable. */
    const Fact *adopt = NULL;
    for (size_t i = 0; i < n && recs[sv].fact.kind == FACT_NONE; i++) {
        if (i != sv && recs[i].fact.kind != FACT_NONE) {
            adopt = &recs[i].fact;
            break;
        }
    }
    const Fact *sv_fact = adopt ? adopt : &recs[sv].fact;

    /* Partition before doing any work. A member asserting something the
     * survivor will not is not part of this merge *at all* — and deciding that
     * here rather than at the tombstone is what stops it donating its tags,
     * weights and edges to a record it is never folded into. */
    unsigned char *keep = calloc(n ? n : 1, 1);
    if (!keep) {
        return 0;
    }
    size_t n_merge = 0;
    for (size_t i = 0; i < n; i++) {
        if (i == sv) {
            keep[i] = 1;
            continue;
        }
        if (recs[i].fact.kind == FACT_NONE ||
            facts_equal(&recs[i].fact, sv_fact)) {
            keep[i] = 1;
            n_merge++;
        } else {
            /* Two records that disagree are not duplicates in the sense that
             * matters, and a merge cannot carry two claims. */
            LOG_DEBUG("consolidate: keeping %llu; it asserts a fact the "
                      "survivor does not",
                      (unsigned long long)recs[i].id);
        }
    }
    if (n_merge == 0) {
        /* Nothing to fold, so write nothing. A cluster that rewrote its
         * survivor anyway would append a record and fsync on every pass
         * forever — the losers are still live, so they re-enter this function
         * next time — and refreshing `updated` each pass would also stop
         * forget's decay clock from ever advancing. */
        free(keep);
        return 0;
    }

    /* union of the merging members' tags + max importance/confidence */
    const char **utags = NULL;
    size_t un = 0;
    size_t ucap = 0;
    float imp = 0;
    float conf = 0;
    for (size_t i = 0; i < n; i++) {
        if (!keep[i]) {
            continue;
        }
        if (recs[i].importance > imp) {
            imp = recs[i].importance;
        }
        if (recs[i].confidence > conf) {
            conf = recs[i].confidence;
        }
        for (size_t t = 0; t < recs[i].tag_count; t++) {
            int seen = 0;
            for (size_t u = 0; u < un; u++) {
                if (strcmp(utags[u], recs[i].tags[t]) == 0) {
                    seen = 1;
                    break;
                }
            }
            if (seen) {
                continue;
            }
            if (un == ucap) {
                size_t nc = ucap ? ucap * 2 : 8;
                const char **g = realloc(utags, nc * sizeof(*g));
                if (!g) {
                    break; /* best-effort: keep the tags gathered so far */
                }
                utags = g;
                ucap = nc;
            }
            utags[un++] = recs[i].tags[t];
        }
    }

    /* migrate the merging losers' relationships onto the survivor */
    for (size_t i = 0; i < n; i++) {
        if (!keep[i] || recs[i].id == survivor) {
            continue;
        }
        for (size_t r = 0; r < recs[i].rel_count; r++) {
            qe_relate(db, survivor, recs[i].relationships[r].to_id,
                      recs[i].relationships[r].kind, ns);
        }
    }

    /* fold the merged tags + fields into the survivor */
    UpdatePatch patch;
    memset(&patch, 0, sizeof(patch));
    patch.has_tags = 1;
    patch.tags = utags;
    patch.tag_count = un;
    patch.has_importance = 1;
    patch.importance = imp;
    patch.has_confidence = 1;
    patch.confidence = conf;
    if (adopt) {
        patch.has_fact = 1;
        patch.fact = adopt;
    }
    MemoryRecord upd;
    aegis_status_t ust = qe_update(db, survivor, &patch, ns, &upd);
    free(utags);
    if (ust != AEGIS_OK) {
        /* The survivor did not take the merged content, so tombstoning the
         * records it was supposed to absorb would lose exactly what this
         * function exists to preserve. Leave the cluster alone; the next pass
         * sees the same shape and tries again. */
        LOG_WARN("consolidate: survivor %llu could not absorb the cluster "
                 "(%d); nothing merged",
                 (unsigned long long)survivor, (int)ust);
        free(keep);
        return 0;
    }
    record_free(&upd);

    /* Tombstone the losers, recording provenance first: the survivor
     * `supersedes` each one, so a merge is auditable lineage rather than silent
     * loss. The link is added while the loser still exists (qe_relate needs
     * both endpoints), then it is deleted. */
    size_t merged = 0;
    for (size_t i = 0; i < n; i++) {
        if (!keep[i] || recs[i].id == survivor) {
            continue;
        }
        qe_relate(db, survivor, recs[i].id, "supersedes", ns);
        /* Noted before the tombstone, while both ids still mean something, so
         * truth maintenance can tell a merged premise from a lost one (ROADMAP
         * 5.3 §6). Reaching here means nothing a conclusion could rest on was
         * lost: the survivor asserts the same triple, or this record asserted
         * none. */
        db_supersede_note(db, recs[i].id, survivor);
        /* Re-point the provenance edges too. The routes on a derived record
         * still name the absorbed id — deliberately, since that is what it was
         * drawn from — but the *edges* are what a later delete walks. */
        repoint_dependents(db, recs[i].id, survivor, ns);

        if (qe_delete(db, recs[i].id, ns) == AEGIS_OK) {
            merged++;
        }
    }
    free(keep);
    return merged;
}

aegis_status_t qe_consolidate(AegisDB *db, const char *ns, float min_similarity,
                              size_t *out_clusters, size_t *out_merged) {
    aegis_status_t st = require_phase(db, 3); /* semantic search */
    if (st != AEGIS_OK) {
        return st;
    }
    *out_clusters = 0;
    *out_merged = 0;

    /* snapshot the live semantic ids (hash scan by type; no record reads) */
    size_t n = 0;
    int oom = 0;
    uint64_t *ids = collect_ids(db, keep_semantic, NULL, &n, &oom);
    if (oom) {
        free(ids);
        return AEGIS_ERR_INTERNAL;
    }

    for (size_t i = 0; i < n; i++) {
        /* Load the record for its embedding. A prior merge may have tombstoned
         * it (it was a loser) — qe_get returns NOT_FOUND and we skip it, which
         * is how cluster members self-exclude without an explicit visited set. */
        MemoryRecord r;
        if (qe_get(db, ids[i], ns, &r) != AEGIS_OK) {
            continue;
        }
        if (r.embedding_dim == 0) {
            record_free(&r);
            continue;
        }

        /* find this record's near-duplicates in its own namespace: a semantic
         * search by its vector, gated at min_similarity (raw cosine) */
        SearchParams p;
        memset(&p, 0, sizeof(p));
        p.embedding = r.embedding;
        p.embedding_dim = r.embedding_dim;
        p.agent_id = ns;
        p.has_type = 1;
        p.type = MEM_SEMANTIC;
        p.has_min_score = 1;
        p.min_score = min_similarity;
        p.top_k = CONSOLIDATE_CAP;
        MemoryRecord *recs = NULL;
        size_t rn = 0;
        aegis_status_t rc = qe_search(db, &p, &recs, &rn);
        record_free(&r); /* after qe_search — p.embedding aliases r.embedding */
        if (rc != AEGIS_OK) {
            continue;
        }

        if (rn >= 2) { /* the record plus at least one duplicate */
            *out_merged += merge_cluster(db, recs, rn, ns);
            (*out_clusters)++;
        }
        for (size_t j = 0; j < rn; j++) {
            record_free(&recs[j]);
        }
        free(recs);
    }
    free(ids);
    atomic_fetch_add_explicit(&db->metrics.memories_merged, *out_merged,
                              memory_order_relaxed);
    return AEGIS_OK;
}

/* Decay-based forgetting (ROADMAP 2.3). A maintenance pass that tombstones aging,
 * low-value records so a long-running corpus (and its in-RAM indexes) plateaus
 * instead of growing without bound. Retention = importance * 0.5^(age/half_life),
 * age measured from `updated`; a record is forgotten when retention <
 * min_retention. Scoped to one type (default episodic — the high-volume,
 * low-individual-value events; curated semantic facts are protected unless the
 * caller opts them in) and to `ns`. dry_run counts without deleting. */
aegis_status_t qe_forget(AegisDB *db, const char *ns, MemoryType type,
                         uint64_t half_life_ms, float min_retention,
                         float usage_weight, int dry_run, size_t max_forget,
                         size_t *out_scanned, size_t *out_forgotten) {
    aegis_status_t st = require_phase(db, 1);
    if (st != AEGIS_OK) {
        return st;
    }
    *out_scanned = 0;
    *out_forgotten = 0;
    if (half_life_ms < MIN_HALF_LIFE_MS) {
        half_life_ms = MIN_HALF_LIFE_MS;
    }

    /* snapshot the live ids of the target type (hash scan; no record reads) */
    size_t n = 0;
    int oom = 0;
    uint64_t *ids = collect_ids(db, keep_type, &type, &n, &oom);
    if (oom) {
        free(ids);
        return AEGIS_ERR_INTERNAL;
    }

    uint64_t now = db_now_ms();
    for (size_t i = 0; i < n; i++) {
        if (max_forget && *out_forgotten >= max_forget) {
            break; /* safety cap */
        }
        MemoryRecord r;
        /* qe_get enforces the namespace (NOT_FOUND for another tenant) and skips
         * anything a prior iteration already tombstoned. */
        if (qe_get(db, ids[i], ns, &r) != AEGIS_OK) {
            continue;
        }
        (*out_scanned)++;
        /* Usage feedback changes retention in two ways, both gated on
         * usage_weight so `usage_weight: 0` reproduces the original scoring
         * exactly:
         *
         * 1. Recency is measured from the last *use*, not just the last write. A
         *    fact written a year ago and recalled yesterday is live knowledge;
         *    ageing it by write time would delete it.
         * 2. Frequently-recalled records resist forgetting, via a saturating
         *    boost — 1 + w·(1 − 1/(1+n)). Bounded on purpose: an unbounded
         *    multiplier would pin whatever happens to be hot forever, and n is
         *    evidence of use, not proof of value. */
        uint64_t basis = r.updated;
        double use_boost = 1.0;
        if (usage_weight > 0.0F && db->usage) {
            uint32_t recalls = 0;
            uint64_t last = 0;
            if (usage_index_get(db->usage, r.id, &recalls, &last) == 0) {
                if (last > basis) {
                    basis = last;
                }
                if (recalls) {
                    use_boost = 1.0 + ((double)usage_weight *
                                       (1.0 - (1.0 / (1.0 + (double)recalls))));
                }
            }
        }
        double age = now > basis ? (double)(now - basis) : 0.0;
        /* 0.5^(age/half_life) == exp(-ln2 * age/half_life) */
        double recency = exp(-0.6931471805599453 * age / (double)half_life_ms);
        double retention = (double)r.importance * recency * use_boost;
        record_free(&r);
        if (retention >= (double)min_retention) {
            continue; /* still worth keeping */
        }
        if (dry_run) {
            (*out_forgotten)++; /* would forget */
        } else if (qe_delete(db, ids[i], ns) == AEGIS_OK) {
            (*out_forgotten)++;
        }
    }
    free(ids);
    if (!dry_run) {
        atomic_fetch_add_explicit(&db->metrics.memories_forgotten,
                                  *out_forgotten, memory_order_relaxed);
    }
    return AEGIS_OK;
}
