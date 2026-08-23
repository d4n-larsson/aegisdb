/* Query engine — wire/JSON handlers, auth, token admin, dispatch table,
 * metrics (split from query_engine.c). Calls the engine via the public qe_ API. */
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

/* ----- dispatch / wire handlers ---------------------------------------- */

/* Ranking breakdown for one hit (ROADMAP 1.2): explains why it ranked here.
 * score == weight * relevance * recency_factor, where relevance is the cosine
 * similarity, the BM25 score, or the fused reciprocal rank (ROADMAP 4.1). Only
 * the fields that contributed are emitted, so a response stays readable. */
static cJSON *search_explain_json(const SearchExplain *e) {
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    cJSON_AddBoolToObject(o, "semantic", e->semantic);
    cJSON_AddBoolToObject(o, "lexical", e->lexical);
    cJSON_AddNumberToObject(o, "score", e->score);
    if (e->semantic) {
        cJSON_AddNumberToObject(o, "similarity", e->similarity);
    }
    if (e->lexical) {
        cJSON_AddNumberToObject(o, "bm25", e->bm25);
    }
    /* Hybrid only (rrf is 0 for a single-source query). Both ranks are always
     * reported, including the zero, so a one-sided match is visible rather than
     * inferred from an absent field: `lexical_rank: 1, semantic_rank: 0` is the
     * case this feature exists for — the exact term found it, the vectors
     * missed it. */
    if (e->rrf > 0) {
        cJSON_AddNumberToObject(o, "semantic_rank", e->semantic_rank);
        cJSON_AddNumberToObject(o, "lexical_rank", e->lexical_rank);
        cJSON_AddNumberToObject(o, "rrf", e->rrf);
    }
    cJSON_AddNumberToObject(o, "importance", e->importance);
    cJSON_AddNumberToObject(o, "confidence", e->confidence);
    cJSON_AddNumberToObject(o, "weight", e->weight);
    cJSON_AddNumberToObject(o, "recency_factor", e->recency_factor);
    return o;
}

/* Attach usage feedback to a record's JSON. The counters live in a side index,
 * not in the record, so they are added here where `db` is in scope rather than
 * inside json_record. Omitted entirely when the server keeps no counters
 * (--no-usage-feedback) or the id is untracked, so a client can tell "never
 * recalled" (0) from "not tracked" (absent). */
static void add_usage(cJSON *jr, AegisDB *db, uint64_t id) {
    if (!jr || !db->usage) {
        return;
    }
    uint32_t count = 0;
    uint64_t last = 0;
    pthread_rwlock_rdlock(&db->index_lock);
    int found = usage_index_get(db->usage, id, &count, &last);
    pthread_rwlock_unlock(&db->index_lock);
    if (found != 0) {
        return;
    }
    cJSON_AddNumberToObject(jr, "recall_count", (double)count);
    if (last) {
        cJSON_AddNumberToObject(jr, "last_recalled", (double)last);
    }
}

static cJSON *resp_record(const MemoryRecord *r, int include_embeddings) {
    cJSON *o = json_ok();
    if (!o) {
        return NULL;
    }
    cJSON_AddItemToObject(o, "record", json_record(r, include_embeddings));
    return o;
}

/* Optional "include_embeddings" response shaping: embeddings are the bulk of a
 * record's JSON (a 384-float vector is ~8 KB), so recall/read clients that only
 * need the payload can set it false. Defaults to true (backward compatible). */
static int want_embeddings(const cJSON *req) {
    return jr_bool(req, "include_embeddings", 1);
}

/* Parse an optional `fact` (ROADMAP 5.2) onto the record being built.
 *   "fact": { "s": <id>, "p": "<predicate>", "o": "<literal>" | {"id": <id>} }
 * Returns 0 (including when absent) or -1 with *err set.
 *
 * The subject and an id-valued object are **not** checked for existence.
 * Mirroring `relate` was tempting, but a batch that inserts an entity and a
 * fact about it in one request would then be refused for referencing a record
 * that is written moments later — and the extractor path this exists for wants
 * exactly that shape. A dangling reference is also not a security question: the
 * fact lives on the *asserting* record, which carries the caller's namespace, so
 * a pattern search still only ever returns records the caller owns. Asserting
 * something about an id you cannot see reveals nothing about it. */
static int build_fact(const cJSON *req, MemoryRecord *in, aegis_status_t *err) {
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(req, "fact");
    if (!f || cJSON_IsNull(f)) {
        return 0;
    }
    if (!cJSON_IsObject(f)) {
        *err = AEGIS_ERR_INVALID_REQUEST;
        return -1;
    }
    uint64_t subject = 0;
    const char *pred = jr_str(f, "p", NULL);
    if (jr_u64(f, "s", &subject) != 0 || !pred || !*pred) {
        *err = AEGIS_ERR_INVALID_REQUEST;
        return -1;
    }
    /* Bounded so the fact indexes can always intern it. Past this the fact
     * would be stored but unreachable by any pattern naming its predicate,
     * which is a worse outcome than refusing the write. */
    if (strlen(pred) > FACT_MAX_PREDICATE_LEN) {
        *err = AEGIS_ERR_INVALID_REQUEST;
        return -1;
    }
    const cJSON *jo = cJSON_GetObjectItemCaseSensitive(f, "o");
    if (cJSON_IsString(jo) && jo->valuestring) {
        if (record_set_fact(in, FACT_OBJ_STRING, subject, pred, 0,
                            jo->valuestring) != 0) {
            *err = AEGIS_ERR_INTERNAL;
            return -1;
        }
        return 0;
    }
    uint64_t oid = 0;
    if (cJSON_IsObject(jo) && jr_u64(jo, "id", &oid) == 0) {
        if (record_set_fact(in, FACT_OBJ_ID, subject, pred, oid, NULL) != 0) {
            *err = AEGIS_ERR_INTERNAL;
            return -1;
        }
        return 0;
    }
    /* A bare number is deliberately not an object: it would be ambiguous
     * between an id reference and a numeric literal, and numeric literals do
     * not exist (see docs/typed-facts-design.md §5). */
    *err = AEGIS_ERR_INVALID_REQUEST;
    return -1;
}

static int build_input_record(AegisDB *db, const cJSON *req, MemoryRecord *in,
                              aegis_status_t *err) {
    record_init(in);
    const char *type = jr_str(req, "type", NULL);
    if (!type || memory_type_from_string(type, &in->type) != 0) {
        *err = AEGIS_ERR_INVALID_REQUEST;
        return -1;
    }
    const char *data = jr_str(req, "data", NULL);
    if (!data) {
        *err = AEGIS_ERR_INVALID_REQUEST;
        return -1;
    }
    in->data_len = strlen(data);
    in->data = malloc(in->data_len ? in->data_len : 1);
    if (!in->data) {
        *err = AEGIS_ERR_INTERNAL;
        return -1;
    }
    memcpy(in->data, data, in->data_len);

    double d;
    if (jr_f64(req, "importance", &d) == 0) {
        in->importance = (float)d;
    }
    if (jr_f64(req, "confidence", &d) == 0) {
        in->confidence = (float)d;
    }
    const char *agent = jr_str(req, "agent_id", NULL);
    if (agent) {
        in->agent_id = strdup(agent);
        if (!in->agent_id) {
            *err = AEGIS_ERR_INTERNAL;
            return -1;
        }
    }
    /* `derivation` is written by the inference job and by nothing else
     * (ROADMAP 5.3). A client that could supply one could manufacture
     * provenance — a record that claims to follow from premises it never
     * followed from — and every trust claim in this horizon rests on that being
     * impossible. Refused rather than ignored, so a caller who sends one learns
     * the field is not theirs instead of watching it vanish. */
    const cJSON *jderiv = cJSON_GetObjectItemCaseSensitive(req, "derivation");
    if (jderiv && !cJSON_IsNull(jderiv)) {
        *err = AEGIS_ERR_INVALID_REQUEST;
        return -1;
    }
    if (build_fact(req, in, err) != 0) {
        return -1;
    }
    const char **tags = NULL;
    size_t tn = 0;
    if (jr_str_array(req, "tags", &tags, &tn, MAX_TAGS) != 0) {
        *err = AEGIS_ERR_INVALID_REQUEST;
        return -1;
    }
    if (tn && record_set_tags(in, tags, tn) != 0) {
        free(tags);
        *err = AEGIS_ERR_INTERNAL;
        return -1;
    }
    free(tags);
    /* Vectors: `embeddings` (array of equal-length arrays) for multi-vector, or
     * `embedding` (a single array) for the common one-vector case (#85). All
     * vectors must equal the server's embedding dimension. */
    size_t dim = db->config.embedding_dimensions;
    const cJSON *embs = cJSON_GetObjectItemCaseSensitive(req, "embeddings");
    if (cJSON_IsArray(embs)) {
        size_t vc = (size_t)cJSON_GetArraySize(embs);
        if (vc == 0 || vc > MAX_VECS_PER_RECORD) {
            *err = AEGIS_ERR_INVALID_REQUEST;
            return -1;
        }
        float *buf = malloc(vc * dim * sizeof(float));
        if (!buf) {
            *err = AEGIS_ERR_INTERNAL;
            return -1;
        }
        size_t w = 0;
        for (size_t v = 0; v < vc; v++) {
            const cJSON *vec = cJSON_GetArrayItem(embs, (int)v);
            if (!cJSON_IsArray(vec) || (size_t)cJSON_GetArraySize(vec) != dim) {
                free(buf);
                *err = AEGIS_ERR_INVALID_REQUEST;
                return -1;
            }
            for (size_t i = 0; i < dim; i++) {
                const cJSON *n = cJSON_GetArrayItem(vec, (int)i);
                if (!cJSON_IsNumber(n)) {
                    free(buf);
                    *err = AEGIS_ERR_INVALID_REQUEST;
                    return -1;
                }
                buf[w++] = (float)n->valuedouble;
            }
        }
        in->embedding = buf;
        in->embedding_dim = dim;
        in->vec_count = vc;
    } else {
        float *emb = NULL;
        size_t en = 0;
        if (jr_float_array(req, "embedding", &emb, &en, dim) != 0) {
            *err = AEGIS_ERR_INVALID_REQUEST;
            return -1;
        }
        if (en) {
            in->embedding = emb;
            in->embedding_dim = en;
            in->vec_count = 1;
        }
    }
    *err = AEGIS_OK;
    return 0;
}

/* Resolved caller identity for a request. The namespace is copied into ns_buf
 * (not aliased into the token table) so it stays valid for the whole request
 * even if a concurrent token_revoke frees the matched token. */
typedef struct {
    char ns_buf[512];
    const char *ns; /* -> ns_buf, or NULL = unrestricted (admin / auth off) */
    int can_write;  /* 0 for read-only tokens */
} AuthCtx;

/* Every dispatch handler takes the same shape so one table can drive dispatch,
 * auth, and metrics. `ctx` is the resolved identity (NULL only for ping, which
 * dispatches before authentication). Handlers that don't need `req`/`ctx`
 * ignore them. */

static cJSON *handle_ping(AegisDB *db, const cJSON *req, const AuthCtx *ctx) {
    (void)req;
    (void)ctx;
    cJSON *o = json_ok();
    cJSON_AddStringToObject(o, "version", AEGIS_VERSION_STRING);
    cJSON_AddNumberToObject(o, "phase", db->config.enabled_phase);
    return o;
}

/* Operational snapshot: durability posture, durability lag, record/index
 * counts. Read-only; intended for monitoring and capacity planning. */
/* Record counts, log size, and per-index counts + resident bytes, captured under
 * the index read lock. */
static void stats_add_storage(cJSON *o, AegisDB *db) {
    pthread_rwlock_rdlock(&db->index_lock);
    size_t live = 0;
    size_t tombstones = 0;
    const HashIndex *h = db->hash;
    for (size_t i = 0; i < h->cap; i++) {
        if (!h->buckets[i].used) {
            continue;
        }
        if (h->buckets[i].deleted) {
            tombstones++;
        } else {
            live++;
        }
    }
    cJSON_AddNumberToObject(o, "records", (double)live);
    cJSON_AddNumberToObject(o, "tombstones", (double)tombstones);
    cJSON_AddNumberToObject(o, "log_bytes", (double)db->log.size);
    cJSON_AddBoolToObject(o, "log_flush_pending", log_flush_pending(&db->log));

    cJSON *idx = cJSON_AddObjectToObject(o, "indexes");
    if (idx) {
        cJSON_AddNumberToObject(idx, "time", (double)db->time->n);
        cJSON_AddNumberToObject(idx, "tags", (double)tag_index_count(db->tags));
        /* Distinct terms and indexed documents; both 0 with --no-lexical-index. */
        cJSON_AddNumberToObject(idx, "lexical_terms",
                                (double)lexical_index_terms(db->lex));
        cJSON_AddNumberToObject(idx, "lexical_docs",
                                (double)lexical_index_docs(db->lex));
        /* Indexed incoming edges and the distinct kinds they carry; both 0
         * with --no-edge-index. */
        cJSON_AddNumberToObject(idx, "edges",
                                (double)edge_index_edges(db->edges));
        cJSON_AddNumberToObject(idx, "edge_kinds",
                                (double)edge_index_kinds(db->edges));
        /* Indexed facts and the distinct predicates they use; both 0 with
         * --no-fact-index. */
        cJSON_AddNumberToObject(idx, "facts",
                                (double)fact_index_facts(db->facts));
        cJSON_AddNumberToObject(idx, "fact_predicates",
                                (double)fact_index_predicates(db->facts));
        /* Inference (ROADMAP 5.3). `derived` counts conclusions written since
         * start rather than live derived records, and is named for what it
         * measures: a live count would need maintaining on every delete, and a
         * number that drifts is worse than one that is honest about its span.
         * `inference_deferred` is the one to alert on — permanently set means
         * the caps are too small for the corpus and the job never reaches
         * fixpoint, which is survivable but worth seeing. */
        cJSON_AddNumberToObject(idx, "derived",
                                (double)atomic_load_explicit(
                                    &db->derived_total, memory_order_relaxed));
        cJSON_AddNumberToObject(idx, "conflicts",
                                (double)atomic_load_explicit(
                                    &db->conflicts_now, memory_order_relaxed));
        cJSON_AddNumberToObject(
            idx, "retracted",
            (double)atomic_load_explicit(&db->retracted_total,
                                         memory_order_relaxed));
        cJSON_AddNumberToObject(idx, "inference_last_ms",
                                (double)atomic_load_explicit(
                                    &db->infer_last_ms, memory_order_relaxed));
        cJSON_AddNumberToObject(idx, "inference_deferred",
                                (double)atomic_load_explicit(
                                    &db->infer_deferred, memory_order_relaxed));
        /* Declared, not in use: 0 means no registry is configured, so every
         * predicate is accepted. Worth being able to confirm from outside. */
        cJSON_AddNumberToObject(
            idx, "registered_predicates",
            (double)predicate_registry_count(db->predicates));
        cJSON_AddNumberToObject(idx, "usage_tracked",
                                (double)usage_index_count(db->usage));
        cJSON_AddNumberToObject(idx, "semantic",
                                (double)semantic_index_count(db->sem));
        cJSON_AddNumberToObject(idx, "working",
                                (double)working_store_count(db->working));
    }

    /* Approximate resident bytes per index. Indexes are held in RAM, so this is
     * the figure to watch/alert on — memory grows with the dataset, and the
     * semantic vectors usually dominate. Excludes allocator overhead. */
    cJSON *mem = cJSON_AddObjectToObject(o, "memory");
    if (mem) {
        size_t hb = hash_index_bytes(db->hash);
        size_t tb = time_index_bytes(db->time);
        size_t gb = tag_index_bytes(db->tags);
        size_t lb = lexical_index_bytes(db->lex);
        size_t eb = edge_index_bytes(db->edges);
        size_t fb = fact_index_bytes(db->facts);
        size_t ub = usage_index_bytes(db->usage);
        size_t sb = semantic_index_bytes(db->sem);
        cJSON_AddNumberToObject(mem, "hash_bytes", (double)hb);
        cJSON_AddNumberToObject(mem, "time_bytes", (double)tb);
        cJSON_AddNumberToObject(mem, "tag_bytes", (double)gb);
        cJSON_AddNumberToObject(mem, "lexical_bytes", (double)lb);
        cJSON_AddNumberToObject(mem, "edge_bytes", (double)eb);
        cJSON_AddNumberToObject(mem, "fact_bytes", (double)fb);
        cJSON_AddNumberToObject(mem, "usage_bytes", (double)ub);
        cJSON_AddNumberToObject(mem, "semantic_bytes", (double)sb);
        cJSON_AddNumberToObject(
            mem, "index_bytes_total",
            (double)(hb + tb + gb + lb + eb + fb + ub + sb));
        /* The configured backpressure cap (0 = unlimited), so a scraper can
         * alert on index_bytes_total approaching it. */
        cJSON_AddNumberToObject(mem, "index_bytes_limit",
                                (double)db->config.max_index_bytes);
    }
    pthread_rwlock_unlock(&db->index_lock);
}

/* Recall-latency distribution (ROADMAP 3.3). Emitted under `metrics` as
 * `recall_latency`, with **cumulative** bucket counts keyed by their upper bound
 * in microseconds — Prometheus `le` semantics, so a scraper can pass them
 * straight through — plus the count, the summed latency, and interpolated
 * percentile estimates for operators reading `stats` by hand rather than through
 * Prometheus.
 *
 * The percentiles are estimates: within the chosen bucket the position is
 * interpolated linearly, which is the same approximation Prometheus'
 * histogram_quantile makes. A value in the overflow bucket has no upper bound to
 * interpolate toward, so it reports the last finite bound (i.e. "at least this
 * slow"). Omitted entirely until the first search, so a fresh server does not
 * report a misleading 0. */
static void stats_add_recall_latency(cJSON *m, Metrics *mt) {
    uint64_t count =
        (uint64_t)atomic_load_explicit(&mt->recall_count, memory_order_relaxed);
    if (!count) {
        return;
    }
    uint64_t buckets[RECALL_HIST_N];
    for (size_t i = 0; i < RECALL_HIST_N; i++) {
        buckets[i] = (uint64_t)atomic_load_explicit(&mt->recall_hist[i],
                                                    memory_order_relaxed);
    }
    cJSON *rl = cJSON_AddObjectToObject(m, "recall_latency");
    if (!rl) {
        return;
    }
    uint64_t total_micros = (uint64_t)atomic_load_explicit(
        &mt->recall_micros, memory_order_relaxed);
    cJSON_AddNumberToObject(rl, "count", (double)count);
    cJSON_AddNumberToObject(rl, "micros_total", (double)total_micros);
    cJSON_AddNumberToObject(rl, "mean_micros",
                            (double)total_micros / (double)count);

    /* Cumulative buckets. The bucket totals are sampled independently above, so
     * a concurrent search can leave the last bucket short of `count`; clamp so
     * the series stays monotone and never exceeds the reported count. */
    cJSON *bk = cJSON_AddObjectToObject(rl, "buckets");
    uint64_t cum = 0;
    uint64_t cums[RECALL_HIST_N];
    char key[32];
    for (size_t i = 0; i < RECALL_HIST_N; i++) {
        cum += buckets[i];
        cums[i] = cum > count ? count : cum;
        if (!bk) {
            continue;
        }
        if (i + 1 < RECALL_HIST_N) {
            snprintf(key, sizeof(key), "%llu",
                     (unsigned long long)recall_hist_bounds[i]);
        } else {
            snprintf(key, sizeof(key), "+Inf");
        }
        cJSON_AddNumberToObject(bk, key, (double)cums[i]);
    }
    /* The +Inf bucket is the observation count by definition. */
    if (bk) {
        cJSON_SetNumberValue(cJSON_GetObjectItem(bk, "+Inf"), (double)count);
    }
    cums[RECALL_HIST_N - 1] = count;

    static const struct {
        const char *name;
        double q;
    } quantiles[] = {
        {"p50_micros", 0.50}, {"p95_micros", 0.95}, {"p99_micros", 0.99}};
    for (size_t qi = 0; qi < sizeof(quantiles) / sizeof(quantiles[0]); qi++) {
        double target = quantiles[qi].q * (double)count;
        double est = (double)recall_hist_bounds[RECALL_HIST_N - 2];
        for (size_t i = 0; i < RECALL_HIST_N; i++) {
            if ((double)cums[i] < target) {
                continue;
            }
            double lo = (i == 0) ? 0.0 : (double)recall_hist_bounds[i - 1];
            if (i + 1 >= RECALL_HIST_N) {
                est = (double)recall_hist_bounds[RECALL_HIST_N - 2];
                break; /* overflow: no upper bound to interpolate toward */
            }
            double hi = (double)recall_hist_bounds[i];
            double below = (i == 0) ? 0.0 : (double)cums[i - 1];
            double in_bucket = (double)cums[i] - below;
            double frac = in_bucket > 0 ? (target - below) / in_bucket : 1.0;
            est = lo + ((hi - lo) * frac);
            break;
        }
        cJSON_AddNumberToObject(rl, quantiles[qi].name, est);
    }
}

/* Monotonic operational counters, for scraping (rates = successive diffs). */
static void stats_add_metrics(cJSON *o, AegisDB *db) {
    static const char *const op_names[MOP__N] = {
        "ping",   "insert",  "get",         "update",   "delete", "search",
        "count",  "promote", "relate",      "traverse", "stats",  "history",
        "export", "purge",   "consolidate", "forget",   "other"};
    Metrics *mt = &db->metrics;
    cJSON *m = cJSON_AddObjectToObject(o, "metrics");
    if (!m) {
        return;
    }
    cJSON_AddNumberToObject(
        m, "requests",
        (double)atomic_load_explicit(&mt->requests, memory_order_relaxed));
    cJSON_AddNumberToObject(
        m, "errors",
        (double)atomic_load_explicit(&mt->errors, memory_order_relaxed));
    cJSON_AddNumberToObject(
        m, "unauthorized",
        (double)atomic_load_explicit(&mt->unauthorized, memory_order_relaxed));
    cJSON_AddNumberToObject(m, "dispatch_micros",
                            (double)atomic_load_explicit(&mt->dispatch_micros,
                                                         memory_order_relaxed));
    cJSON_AddNumberToObject(m, "memories_merged",
                            (double)atomic_load_explicit(&mt->memories_merged,
                                                         memory_order_relaxed));
    cJSON_AddNumberToObject(m, "memories_forgotten",
                            (double)atomic_load_explicit(
                                &mt->memories_forgotten, memory_order_relaxed));
    cJSON_AddNumberToObject(m, "memories_purged",
                            (double)atomic_load_explicit(&mt->memories_purged,
                                                         memory_order_relaxed));
    cJSON *bo = cJSON_AddObjectToObject(m, "by_op");
    if (bo) {
        for (int i = 0; i < MOP__N; i++) {
            cJSON_AddNumberToObject(bo, op_names[i],
                                    (double)atomic_load_explicit(
                                        &mt->by_op[i], memory_order_relaxed));
        }
    }
    stats_add_recall_latency(m, mt);
}

/* Per-tenant usage against the configured caps (admin-only op, so this is
 * server-wide). Only emitted when a limit is configured, keeping the common
 * single-tenant / no-quota response unchanged. */
static void stats_add_tenants(cJSON *o, AegisDB *db) {
    if (!(db->config.tenant_max_records || db->config.tenant_max_bytes ||
          db->config.tenant_rate_qps > 0)) {
        return;
    }
    cJSON *lim = cJSON_AddObjectToObject(o, "tenant_limits");
    if (lim) {
        cJSON_AddNumberToObject(lim, "max_records",
                                (double)db->config.tenant_max_records);
        cJSON_AddNumberToObject(lim, "max_bytes",
                                (double)db->config.tenant_max_bytes);
        cJSON_AddNumberToObject(lim, "rate_qps", db->config.tenant_rate_qps);
    }
    size_t tn = 0;
    TenantUsage *usage = tenant_usage_snapshot(db->tenants, &tn);
    cJSON *arr = cJSON_AddArrayToObject(o, "tenants");
    for (size_t i = 0; arr && i < tn; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "namespace", usage[i].ns);
        cJSON_AddNumberToObject(e, "records", (double)usage[i].records);
        cJSON_AddNumberToObject(e, "bytes", (double)usage[i].bytes);
        cJSON_AddItemToArray(arr, e);
    }
    tenant_usage_free(usage, tn);
}

/* Replication posture (only when this node participates). */
static void stats_add_replication(cJSON *o, AegisDB *db) {
    if (db->repl_follower) {
        uint64_t applied = 0;
        uint64_t primary_size = 0;
        int connected = 0;
        replication_follower_status(db->repl_follower, &applied, &primary_size,
                                    &connected);
        cJSON *rep = cJSON_AddObjectToObject(o, "replication");
        if (rep) {
            cJSON_AddStringToObject(rep, "role", "replica");
            cJSON_AddBoolToObject(rep, "connected", connected);
            cJSON_AddNumberToObject(rep, "applied_offset", (double)applied);
            cJSON_AddNumberToObject(rep, "primary_offset",
                                    (double)primary_size);
            cJSON_AddNumberToObject(
                rep, "lag_bytes",
                (double)(primary_size > applied ? primary_size - applied : 0));
        }
    } else if (db->repl_source) {
        cJSON *rep = cJSON_AddObjectToObject(o, "replication");
        if (rep) {
            cJSON_AddStringToObject(rep, "role", "primary");
            cJSON_AddNumberToObject(
                rep, "replicas",
                (double)replication_source_replica_count(db->repl_source));
            cJSON_AddNumberToObject(
                rep, "log_generation",
                (double)atomic_load_explicit(&db->log_generation,
                                             memory_order_relaxed));
        }
    }
}

static cJSON *handle_stats(AegisDB *db, const cJSON *req, const AuthCtx *ctx) {
    (void)req;
    (void)ctx;
    cJSON *o = json_ok();
    cJSON_AddStringToObject(o, "version", AEGIS_VERSION_STRING);
    cJSON_AddNumberToObject(o, "phase", db->config.enabled_phase);
    cJSON_AddNumberToObject(o, "uptime_ms",
                            (double)(db_now_ms() - db->started_ms));
    cJSON_AddStringToObject(o, "durability",
                            aegis_durability_name(db->config.durability));
    if (db->config.durability == AEGIS_DURABILITY_BATCH) {
        cJSON_AddNumberToObject(o, "fsync_batch",
                                (double)db->config.fsync_batch_size);
    } else if (db->config.durability == AEGIS_DURABILITY_INTERVAL) {
        cJSON_AddNumberToObject(o, "fsync_interval_ms",
                                (double)db->config.fsync_interval_ms);
    }

    stats_add_storage(o, db);

    pthread_mutex_lock(&db->id_lock);
    cJSON_AddNumberToObject(o, "next_id", (double)db->next_id);
    pthread_mutex_unlock(&db->id_lock);

    stats_add_metrics(o, db);
    stats_add_tenants(o, db);
    stats_add_replication(o, db);
    return o;
}

/* Admin: write a consistent online snapshot (backup) of the log under
 * <data_dir>/snapshots/<name>/. `name` defaults to snap-<epoch_ms>. Returns the
 * snapshot location and what it covers. */
static cJSON *handle_snapshot(AegisDB *db, const cJSON *req,
                              const AuthCtx *ctx) {
    (void)ctx;
    const char *name = jr_str(req, "name", NULL);
    char autobuf[40];
    if (!name) {
        snprintf(autobuf, sizeof(autobuf), "snap-%llu",
                 (unsigned long long)db_now_ms());
        name = autobuf;
    }
    DbSnapshotInfo info;
    int rc = db_snapshot(db, name, &info);
    if (rc == DB_SNAPSHOT_BADNAME) {
        return json_error("INVALID_REQUEST", "invalid snapshot name");
    }
    if (rc != DB_SNAPSHOT_OK) {
        return json_error_status(AEGIS_ERR_INTERNAL);
    }

    cJSON *o = json_ok();
    cJSON_AddStringToObject(o, "snapshot", info.dir);
    cJSON_AddNumberToObject(o, "log_size", (double)info.log_size);
    cJSON_AddNumberToObject(o, "record_count", (double)info.record_count);
    cJSON_AddNumberToObject(o, "next_id", (double)info.next_id);
    cJSON_AddNumberToObject(o, "created_ms", (double)info.created_ms);
    return o;
}

static int parse_filters(const cJSON *req, const char *ns, SearchParams *p,
                         const char ***out_tags);
static aegis_status_t parse_pattern(AegisDB *db, const cJSON *req,
                                    SearchParams *p);

/* Is a `pattern` present at all? Used by the ops that share SearchParams but do
 * not honour it, so they refuse rather than silently drop a filter the caller
 * asked for — the same reasoning `direction` and `kinds` follow on traverse. */
static int has_pattern_field(const cJSON *req) {
    const cJSON *pat = cJSON_GetObjectItemCaseSensitive(req, "pattern");
    return pat && !cJSON_IsNull(pat);
}

#define MAX_BATCH 1000 /* max records per batch insert (bounds work/allocs) */

/* Parse `search`'s `pattern` filter (ROADMAP 5.2) into p.
 *   "pattern": { "s": <id>|"*", "p": "<pred>"|"*", "o": "<lit>"|{"id":N}|"*" }
 * An omitted or "*" position is a wildcard. Returns AEGIS_OK, or an error: at
 * least one position must be bound, since an all-wildcard pattern is a scan of
 * every fact wearing a filter's clothes. */
static aegis_status_t parse_pattern(AegisDB *db, const cJSON *req,
                                    SearchParams *p) {
    const cJSON *pat = cJSON_GetObjectItemCaseSensitive(req, "pattern");
    if (!pat || cJSON_IsNull(pat)) {
        return AEGIS_OK;
    }
    if (!cJSON_IsObject(pat)) {
        return AEGIS_ERR_INVALID_REQUEST;
    }
    /* Reported before parsing, so a client learns the server cannot answer
     * patterns at all rather than that its particular pattern was malformed. */
    if (!db->facts) {
        return AEGIS_ERR_NOT_READY;
    }
    p->has_pattern = 1;

    const cJSON *js = cJSON_GetObjectItemCaseSensitive(pat, "s");
    if (js && !cJSON_IsNull(js)) {
        if (cJSON_IsNumber(js)) {
            /* Through jr_u64, not a bare cast: converting a negative or
             * >= 2^64 double to uint64_t is undefined, and in practice yields
             * a garbage subject that silently matches the wrong facts (or
             * traps, under a -fsanitize=float-cast-overflow build). The object
             * position below already goes through it. */
            if (jr_u64(pat, "s", &p->pat_subject) != 0) {
                return AEGIS_ERR_INVALID_REQUEST;
            }
            p->pat_has_subject = 1;
        } else if (!(cJSON_IsString(js) && js->valuestring &&
                     strcmp(js->valuestring, "*") == 0)) {
            return AEGIS_ERR_INVALID_REQUEST;
        }
    }
    const cJSON *jp = cJSON_GetObjectItemCaseSensitive(pat, "p");
    if (jp && !cJSON_IsNull(jp)) {
        if (!cJSON_IsString(jp) || !jp->valuestring || !*jp->valuestring) {
            return AEGIS_ERR_INVALID_REQUEST;
        }
        if (strcmp(jp->valuestring, "*") != 0) {
            p->pat_predicate = jp->valuestring; /* borrowed from the request */
        }
    }
    const cJSON *jo = cJSON_GetObjectItemCaseSensitive(pat, "o");
    if (jo && !cJSON_IsNull(jo)) {
        if (cJSON_IsString(jo) && jo->valuestring) {
            if (strcmp(jo->valuestring, "*") != 0) {
                p->pat_has_object = 1;
                p->pat_object_kind = FACT_OBJ_STRING;
                p->pat_object_str = jo->valuestring;
            }
        } else if (cJSON_IsObject(jo)) {
            uint64_t oid = 0;
            if (jr_u64(jo, "id", &oid) != 0) {
                return AEGIS_ERR_INVALID_REQUEST;
            }
            p->pat_has_object = 1;
            p->pat_object_kind = FACT_OBJ_ID;
            p->pat_object_id = oid;
        } else {
            return AEGIS_ERR_INVALID_REQUEST;
        }
    }
    if (!p->pat_has_subject && !p->pat_predicate && !p->pat_has_object) {
        return AEGIS_ERR_INVALID_REQUEST;
    }
    return AEGIS_OK;
}

/* Build one input record from `spec`, pin it to `ns` when set. 0/-1 via *err. */
static int build_pinned(AegisDB *db, const cJSON *spec, const char *ns,
                        MemoryRecord *in, aegis_status_t *err) {
    if (build_input_record(db, spec, in, err) != 0) {
        return -1;
    }
    if (ns) { /* a namespaced token writes only into its own tenant */
        free(in->agent_id);
        in->agent_id = strdup(ns);
        if (!in->agent_id) {
            *err = AEGIS_ERR_INTERNAL;
            return -1;
        }
    }
    return 0;
}

/* Insert `records`: [ {record}, ... ] in one request. Validates every element
 * first, so a malformed element rejects the whole batch before anything is
 * written; then inserts all. Returns {ok, count, records:[...]}. */
static cJSON *handle_insert_batch(AegisDB *db, const cJSON *arr, const char *ns,
                                  int include_embeddings) {
    int n = cJSON_GetArraySize(arr);
    if (n < 1 || n > MAX_BATCH) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }

    MemoryRecord *ins = calloc((size_t)n, sizeof(MemoryRecord));
    if (!ins) {
        return json_error_status(AEGIS_ERR_INTERNAL);
    }
    for (int i = 0; i < n; i++) {
        aegis_status_t err;
        if (build_pinned(db, cJSON_GetArrayItem(arr, i), ns, &ins[i], &err) !=
            0) {
            record_free(&ins[i]);
            for (int j = 0; j < i; j++) {
                record_free(&ins[j]);
            }
            free(ins);
            return json_error_status(err); /* nothing written yet */
        }
    }

    cJSON *o = json_ok();
    cJSON *out_arr = cJSON_AddArrayToObject(o, "records");
    size_t ok = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *spec = cJSON_GetArrayItem(arr, i);
        const char *session = jr_str(spec, "session_id", NULL);
        uint64_t ttl = 0;
        jr_u64(spec, "ttl_ms", &ttl);
        MemoryRecord out;
        if (qe_insert(db, &ins[i], session, ttl, &out) == AEGIS_OK) {
            cJSON_AddItemToArray(out_arr,
                                 json_record(&out, include_embeddings));
            record_free(&out);
            ok++;
        }
        record_free(&ins[i]);
    }
    free(ins);
    cJSON_AddNumberToObject(o, "count", (double)ok);
    return o;
}

static cJSON *handle_insert(AegisDB *db, const cJSON *req, const AuthCtx *ctx) {
    const char *ns = ctx->ns;
    int emb = want_embeddings(req);
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(req, "records");
    if (cJSON_IsArray(arr)) {
        return handle_insert_batch(db, arr, ns, emb);
    }

    MemoryRecord in;
    aegis_status_t err;
    if (build_pinned(db, req, ns, &in, &err) != 0) {
        record_free(&in);
        return json_error_status(err);
    }
    const char *session = jr_str(req, "session_id", NULL);
    uint64_t ttl = 0;
    jr_u64(req, "ttl_ms", &ttl);
    MemoryRecord out;
    aegis_status_t st = qe_insert(db, &in, session, ttl, &out);
    record_free(&in);
    if (st != AEGIS_OK) {
        return json_error_status(st);
    }
    cJSON *r = resp_record(&out, emb);
    record_free(&out);
    return r;
}

static cJSON *handle_get(AegisDB *db, const cJSON *req, const AuthCtx *ctx) {
    const char *ns = ctx->ns;
    uint64_t id;
    if (jr_u64(req, "id", &id) != 0) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    const char *agent = ns ? ns : jr_str(req, "agent_id", NULL);
    MemoryRecord out;
    /* Point-in-time get (ROADMAP 3.1): `as_of` (epoch ms) returns the record as
     * it was at that time, reconstructed from the log; absent = current. */
    uint64_t as_of;
    aegis_status_t st = (jr_u64(req, "as_of", &as_of) == 0)
                            ? qe_get_as_of(db, id, agent, as_of, &out)
                            : qe_get(db, id, agent, &out);
    if (st != AEGIS_OK) {
        return json_error_status(st);
    }
    cJSON *r = resp_record(&out, want_embeddings(req));
    /* `get` reports the counters but does not increment them: fetching a known id
     * is not retrieval, and counting it would let a tool that walks ids inflate
     * every record's apparent value. */
    add_usage(cJSON_GetObjectItem(r, "record"), db, id);
    record_free(&out);
    return r;
}

static cJSON *handle_history(AegisDB *db, const cJSON *req,
                             const AuthCtx *ctx) {
    const char *ns = ctx->ns;
    uint64_t id;
    if (jr_u64(req, "id", &id) != 0) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    const char *agent = ns ? ns : jr_str(req, "agent_id", NULL);
    MemoryRecord *vers = NULL;
    size_t n = 0;
    aegis_status_t st = qe_history(db, id, agent, &vers, &n);
    if (st != AEGIS_OK) {
        return json_error_status(st);
    }

    int inc_emb = want_embeddings(req);
    cJSON *o = json_ok();
    cJSON_AddNumberToObject(o, "id", (double)id);
    cJSON *arr = cJSON_AddArrayToObject(o, "versions");
    for (size_t i = 0; i < n; i++) {
        cJSON *v = json_record(&vers[i], inc_emb);
        /* validity interval: [updated, next version's updated); 0 = still current */
        cJSON_AddNumberToObject(v, "valid_from", (double)vers[i].updated);
        cJSON_AddNumberToObject(v, "valid_to",
                                (double)(i + 1 < n ? vers[i + 1].updated : 0));
        cJSON_AddBoolToObject(v, "deleted", vers[i].deleted);
        cJSON_AddItemToArray(arr, v);
        record_free(&vers[i]);
    }
    free(vers);
    cJSON_AddNumberToObject(o, "count", (double)n);
    return o;
}

static cJSON *handle_update(AegisDB *db, const cJSON *req, const AuthCtx *ctx) {
    const char *ns = ctx->ns;
    uint64_t id;
    if (jr_u64(req, "id", &id) != 0) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    /* A fact is immutable: changing what a record asserts is a supersession,
     * not an edit (see build_fact). Refused rather than ignored, for the reason
     * bulk `delete` refuses a `pattern` — a silent no-op on a field the caller
     * spelled correctly is worse than either doing it or saying no. */
    const cJSON *jfact = cJSON_GetObjectItemCaseSensitive(req, "fact");
    if (jfact && !cJSON_IsNull(jfact)) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    /* Nor a derivation, for the stronger reason: it is server-written, so an
     * update is the second way a client might try to author one. */
    const cJSON *jderiv = cJSON_GetObjectItemCaseSensitive(req, "derivation");
    if (jderiv && !cJSON_IsNull(jderiv)) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    UpdatePatch patch;
    memset(&patch, 0, sizeof(patch));
    const char *data = jr_str(req, "data", NULL);
    if (data) {
        patch.has_data = 1;
        patch.data = data;
        patch.data_len = strlen(data);
    }
    double d;
    if (jr_f64(req, "importance", &d) == 0) {
        patch.has_importance = 1;
        patch.importance = (float)d;
    }
    if (jr_f64(req, "confidence", &d) == 0) {
        patch.has_confidence = 1;
        patch.confidence = (float)d;
    }
    const char **tags = NULL;
    size_t tn = 0;
    if (cJSON_GetObjectItemCaseSensitive(req, "tags")) {
        if (jr_str_array(req, "tags", &tags, &tn, MAX_TAGS) != 0) {
            return json_error_status(AEGIS_ERR_INVALID_REQUEST);
        }
        patch.has_tags = 1;
        patch.tags = tags;
        patch.tag_count = tn;
    }
    MemoryRecord out;
    aegis_status_t st = qe_update(db, id, &patch, ns, &out);
    free(tags);
    if (st != AEGIS_OK) {
        return json_error_status(st);
    }
    cJSON *r = resp_record(&out, want_embeddings(req));
    record_free(&out);
    return r;
}

static cJSON *handle_delete(AegisDB *db, const cJSON *req, const AuthCtx *ctx) {
    const char *ns = ctx->ns;
    uint64_t id;
    if (jr_u64(req, "id", &id) == 0) {
        aegis_status_t st = qe_delete(db, id, ns);
        if (st != AEGIS_OK) {
            return json_error_status(st);
        }
        cJSON *o = json_ok();
        cJSON_AddNumberToObject(o, "id", (double)id);
        cJSON_AddBoolToObject(o, "deleted", 1);
        return o;
    }
    /* no id: delete every record matching the filters (requires >=1 filter) */
    /* A `pattern` is refused rather than ignored. Deleting by pattern is a
     * coherent operation and may well be worth having, but it is a new
     * destructive capability — not something to acquire as a side effect of the
     * release that added the read filter. Silently dropping it would be worse
     * than either choice. */
    if (has_pattern_field(req)) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    SearchParams p;
    const char **tags = NULL;
    if (parse_filters(req, ns, &p, &tags) != 0) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    size_t deleted = 0;
    aegis_status_t st = qe_delete_by_query(db, &p, ns, &deleted);
    free(tags);
    if (st != AEGIS_OK) {
        return json_error_status(st);
    }
    cJSON *o = json_ok();
    cJSON_AddNumberToObject(o, "deleted", (double)deleted);
    return o;
}

static cJSON *handle_export(AegisDB *db, const cJSON *req, const AuthCtx *ctx) {
    const char *ns = ctx->ns;
    /* Subject = the token's namespace, or an admin-specified agent_id. Refuse a
     * subjectless export (that would be "dump the whole DB"). */
    const char *target = ns ? ns : jr_str(req, "agent_id", NULL);
    if (!target || !*target) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    uint64_t v;
    uint64_t after = 0;
    if (jr_u64(req, "after_id", &v) == 0) {
        after = v;
    }
    size_t limit = 100;
    if (jr_u64(req, "limit", &v) == 0) {
        limit = v > 1000 ? 1000 : (size_t)v;
    }

    MemoryRecord *recs = NULL;
    size_t n = 0;
    int more = 0;
    aegis_status_t st = qe_export(db, target, after, limit, &recs, &n, &more);
    if (st != AEGIS_OK) {
        return json_error_status(st);
    }

    int inc_emb = want_embeddings(req);
    cJSON *o = json_ok();
    cJSON_AddStringToObject(o, "namespace", target);
    cJSON *arr = cJSON_AddArrayToObject(o, "records");
    uint64_t cursor = after;
    for (size_t i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, json_record(&recs[i], inc_emb));
        cursor = recs[i].id;
        record_free(&recs[i]);
    }
    free(recs);
    cJSON_AddNumberToObject(o, "count", (double)n);
    cJSON_AddNumberToObject(o, "cursor",
                            (double)cursor); /* pass as next after_id */
    cJSON_AddBoolToObject(o, "has_more", more);
    return o;
}

static cJSON *handle_purge(AegisDB *db, const cJSON *req, const AuthCtx *ctx) {
    const char *ns = ctx->ns;
    const char *target = ns ? ns : jr_str(req, "agent_id", NULL);
    if (!target || !*target) {
        return json_error_status(
            AEGIS_ERR_INVALID_REQUEST); /* no global purge */
    }
    int dry_run = jr_bool(req, "dry_run", 0);
    int compact = jr_bool(req, "compact", 1);

    size_t purged = 0;
    aegis_status_t st = qe_purge_namespace(db, target, dry_run, &purged);
    if (st != AEGIS_OK) {
        return json_error_status(st);
    }
    /* Compact so the tombstoned payloads actually leave the on-disk log — the
     * point of right-to-be-forgotten. Skippable for a batched/scheduled compact. */
    int compacted = 0;
    if (!dry_run && compact && purged > 0) {
        compacted = (compaction_run_once(db) == 0);
    }

    cJSON *o = json_ok();
    cJSON_AddStringToObject(o, "namespace", target);
    cJSON_AddNumberToObject(o, "purged", (double)purged);
    cJSON_AddBoolToObject(o, "dry_run", dry_run);
    cJSON_AddBoolToObject(o, "compacted", compacted);
    return o;
}

/* Parse the shared filter fields (type/tags/time/agent_id/match) into `p` for
 * count and delete-by-query. On success *out_tags is the allocated tag array
 * (free it after use, even when tag_count is 0). Returns 0/-1. */
static int parse_filters(const cJSON *req, const char *ns, SearchParams *p,
                         const char ***out_tags) {
    memset(p, 0, sizeof(*p));
    if (jr_u64(req, "start_time", &p->start_time) == 0 &&
        jr_u64(req, "end_time", &p->end_time) == 0) {
        p->has_time = 1;
    }
    const char *type = jr_str(req, "type", NULL);
    if (type && memory_type_from_string(type, &p->type) == 0) {
        p->has_type = 1;
    }
    double ms;
    if (jr_f64(req, "max_importance", &ms) == 0) {
        p->has_max_importance = 1;
        p->max_importance = (float)ms;
    }
    p->agent_id = ns ? ns : jr_str(req, "agent_id", NULL);
    const char *match = jr_str(req, "match", "all");
    p->match_all = (strcmp(match, "any") != 0);
    const char **tags = NULL;
    size_t tn = 0;
    if (jr_str_array(req, "tags", &tags, &tn, MAX_TAGS) != 0) {
        return -1;
    }
    p->tags = tags;
    p->tag_count = tn;
    *out_tags = tags;
    return 0;
}

static cJSON *handle_count(AegisDB *db, const cJSON *req, const AuthCtx *ctx) {
    const char *ns = ctx->ns;
    SearchParams p;
    const char **tags = NULL;
    if (parse_filters(req, ns, &p, &tags) != 0) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    /* `count` shares the candidate path, so a pattern narrows it for free —
     * "how many records assert this?" is the obvious companion to the search. */
    aegis_status_t pst = parse_pattern(db, req, &p);
    if (pst != AEGIS_OK) {
        free(tags);
        return json_error_status(pst);
    }
    size_t count = 0;
    int capped = 0;
    aegis_status_t st = qe_count(db, &p, &count, &capped);
    free(tags);
    if (st != AEGIS_OK) {
        return json_error_status(st);
    }
    cJSON *o = json_ok();
    cJSON_AddNumberToObject(o, "count", (double)count);
    /* Flag a count that hit the broad-scan cap so callers don't treat the
     * bounded floor as the true total. */
    if (capped) {
        cJSON_AddBoolToObject(o, "capped", 1);
    }
    return o;
}

#define CONSOLIDATE_DEFAULT_MIN 0.95 /* conservative near-duplicate threshold */

static cJSON *handle_consolidate(AegisDB *db, const cJSON *req,
                                 const AuthCtx *ctx) {
    const char *ns = ctx->ns;
    double ms;
    float min_sim = (jr_f64(req, "min_similarity", &ms) == 0)
                        ? (float)ms
                        : (float)CONSOLIDATE_DEFAULT_MIN;
    size_t clusters = 0;
    size_t merged = 0;
    aegis_status_t st = qe_consolidate(db, ns, min_sim, &clusters, &merged);
    if (st != AEGIS_OK) {
        return json_error_status(st);
    }
    cJSON *o = json_ok();
    cJSON_AddNumberToObject(o, "clusters", (double)clusters);
    cJSON_AddNumberToObject(o, "merged", (double)merged);
    return o;
}

#define FORGET_DEFAULT_HALF_LIFE_MS 604800000ull /* 7 days */
#define FORGET_DEFAULT_MIN_RETENTION 0.05f
/* A recalled record can be worth at most twice an equivalent never-recalled one.
 * Enough to visibly protect what is in use, small enough that importance still
 * decides between two records with similar histories. */
#define FORGET_DEFAULT_USAGE_WEIGHT 1.0f

static cJSON *handle_forget(AegisDB *db, const cJSON *req, const AuthCtx *ctx) {
    const char *ns = ctx->ns;
    uint64_t v;
    double d;
    uint64_t half_life = (jr_u64(req, "half_life_ms", &v) == 0 && v > 0)
                             ? v
                             : FORGET_DEFAULT_HALF_LIFE_MS;
    float min_ret = (jr_f64(req, "min_retention", &d) == 0)
                        ? (float)d
                        : FORGET_DEFAULT_MIN_RETENTION;
    /* Default to episodic only: the high-volume, low-individual-value events.
     * Curated semantic facts are protected unless the caller names the type. */
    MemoryType type = MEM_EPISODIC;
    const char *ts = jr_str(req, "type", NULL);
    if (ts && memory_type_from_string(ts, &type) != 0) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    int dry_run = jr_bool(req, "dry_run", 0);
    size_t max_forget = (jr_u64(req, "max_forget", &v) == 0) ? (size_t)v : 0;
    /* How much a record's recall history protects it. Defaults on, since scoring
     * on a write-time importance guess while ignoring what is actually retrieved
     * is the weakness this exists to fix; 0 restores the pre-feature scoring. */
    float usage_weight = (jr_f64(req, "usage_weight", &d) == 0)
                             ? (float)d
                             : FORGET_DEFAULT_USAGE_WEIGHT;
    if (usage_weight < 0.0F) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }

    size_t scanned = 0;
    size_t forgotten = 0;
    aegis_status_t st =
        qe_forget(db, ns, type, half_life, min_ret, usage_weight, dry_run,
                  max_forget, &scanned, &forgotten);
    if (st != AEGIS_OK) {
        return json_error_status(st);
    }
    cJSON *o = json_ok();
    cJSON_AddNumberToObject(o, "scanned", (double)scanned);
    cJSON_AddNumberToObject(o, "forgotten", (double)forgotten);
    cJSON_AddBoolToObject(o, "dry_run", dry_run);
    cJSON_AddNumberToObject(o, "usage_weight", usage_weight);
    return o;
}

static cJSON *handle_search(AegisDB *db, const cJSON *req, const AuthCtx *ctx) {
    const char *ns = ctx->ns;
    SearchParams p;
    memset(&p, 0, sizeof(p));
    uint64_t v;
    if (jr_u64(req, "start_time", &p.start_time) == 0 &&
        jr_u64(req, "end_time", &p.end_time) == 0) {
        p.has_time = 1;
    }
    const char *type = jr_str(req, "type", NULL);
    if (type && memory_type_from_string(type, &p.type) == 0) {
        p.has_type = 1;
    }
    /* A namespaced token sees only its tenant; admin/no-auth may filter freely. */
    p.agent_id = ns ? ns : jr_str(req, "agent_id", NULL);
    const char *match = jr_str(req, "match", "all");
    p.match_all = (strcmp(match, "any") != 0);
    if (jr_u64(req, "top_k", &v) == 0) {
        p.top_k = v > MAX_TOP_K ? MAX_TOP_K : (size_t)v; /* bound work/allocs */
    }
    if (jr_u64(req, "offset", &v) == 0) {
        p.offset = v > MAX_OFFSET ? MAX_OFFSET : (size_t)v; /* pagination */
    }
    double ms;
    if (jr_f64(req, "min_score", &ms) == 0) {
        p.has_min_score = 1;
        p.min_score = (float)ms;
    }
    if (jr_u64(req, "half_life_ms", &v) == 0 && v > 0) {
        p.half_life_ms =
            v < MIN_HALF_LIFE_MS ? MIN_HALF_LIFE_MS : v; /* 0/absent = off */
    }
    if (jr_f64(req, "max_importance", &ms) == 0) {
        p.has_max_importance = 1;
        p.max_importance = (float)ms;
    }
    /* order=oldest keeps the aging tail when a bounded scan truncates (candidate
     * selection); absent/anything else = default recent-biased scan. */
    p.oldest_first = (strcmp(jr_str(req, "order", ""), "oldest") == 0);
    /* Lexical (BM25) query text; fused with `embedding` when both are present
     * (ROADMAP 4.1). Borrows the cJSON string for the life of the call. */
    p.query = jr_str(req, "query", NULL);
    /* Count these hits as recalls (usage feedback). Defaults on so the signal
     * accrues without every client opting in; a browser of memories — the
     * inspector — passes false so paging through them does not mark them used. */
    p.track_usage = jr_bool(req, "track_usage", 1);
    /* qe_search_ex also rejects this, but with the generic NOT_READY message
     * about phases — which points at the wrong cause. Say what actually needs
     * changing, since the operator has to restart the server to fix it. */
    if (p.query && *p.query && !db->lex) {
        return json_error("NOT_READY",
                          "lexical index is disabled (--no-lexical-index), so "
                          "`query` cannot be served");
    }

    const char **tags = NULL;
    size_t tn = 0;
    if (jr_str_array(req, "tags", &tags, &tn, MAX_TAGS) != 0) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    p.tags = tags;
    p.tag_count = tn;
    float *emb = NULL;
    size_t en = 0;
    if (jr_float_array(req, "embedding", &emb, &en,
                       db->config.embedding_dimensions) != 0) {
        free(tags);
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    p.embedding = emb;
    p.embedding_dim = en;

    aegis_status_t pst = parse_pattern(db, req, &p);
    if (pst != AEGIS_OK) {
        free(tags);
        free(emb);
        return json_error_status(pst);
    }

    /* Opt-in per-hit ranking breakdown (ROADMAP 1.2): off by default so normal
     * responses stay lean; clients/inspection UIs pass "explain": true. */
    int explain = jr_bool(req, "explain", 0);
    MemoryRecord *recs = NULL;
    SearchExplain *expl = NULL;
    size_t n = 0;
    aegis_status_t st = qe_search_ex(db, &p, &recs, explain ? &expl : NULL, &n);
    free(tags);
    free(emb);
    if (st != AEGIS_OK) {
        return json_error_status(st);
    }

    int inc_emb = want_embeddings(req);
    cJSON *o = json_ok();
    cJSON *arr = cJSON_AddArrayToObject(o, "records");
    for (size_t i = 0; i < n; i++) {
        cJSON *jr = json_record(&recs[i], inc_emb);
        add_usage(jr, db, recs[i].id);
        if (explain && expl) {
            cJSON_AddItemToObject(jr, "explain", search_explain_json(&expl[i]));
        }
        cJSON_AddItemToArray(arr, jr);
        record_free(&recs[i]);
    }
    free(recs);
    free(expl);
    cJSON_AddNumberToObject(o, "total", (double)n);
    return o;
}

static cJSON *handle_promote(AegisDB *db, const cJSON *req,
                             const AuthCtx *ctx) {
    const char *ns = ctx->ns;
    const char *session = jr_str(req, "session_id", NULL);
    uint64_t wid;
    if (jr_u64(req, "working_id", &wid) != 0) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    const char *to = jr_str(req, "to_type", "episodic");
    MemoryType tt;
    if (memory_type_from_string(to, &tt) != 0) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    MemoryRecord out;
    aegis_status_t st = qe_promote(db, session, wid, tt, ns, &out);
    if (st != AEGIS_OK) {
        return json_error_status(st);
    }
    cJSON *r = resp_record(&out, want_embeddings(req));
    record_free(&out);
    return r;
}

static cJSON *handle_relate(AegisDB *db, const cJSON *req, const AuthCtx *ctx) {
    const char *ns = ctx->ns;
    uint64_t from;
    uint64_t to;
    if (jr_u64(req, "from_id", &from) != 0 || jr_u64(req, "to_id", &to) != 0) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    const char *kind = jr_str(req, "kind", NULL);
    /* qe_relate enforces that both endpoints live in the caller's namespace */
    aegis_status_t st = qe_relate(db, from, to, kind, ns);
    if (st != AEGIS_OK) {
        return json_error_status(st);
    }
    cJSON *o = json_ok();
    cJSON *rel = cJSON_AddObjectToObject(o, "relationship");
    cJSON_AddNumberToObject(rel, "from_id", (double)from);
    cJSON_AddNumberToObject(rel, "to_id", (double)to);
    if (kind) {
        cJSON_AddStringToObject(rel, "kind", kind);
    }
    return o;
}

/* Which edge reached this hop (ROADMAP 5.1). Emitted for every returned record
 * so a walk reads as a path rather than a set whose shape must be inferred. The
 * start record has no reaching edge, so it reports only its depth. */
static cJSON *traversal_json(const TraverseHop *h) {
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "depth", h->depth);
    if (h->depth > 0) {
        cJSON_AddNumberToObject(o, "via_id", (double)h->via_id);
        /* Absent rather than null when the edge carries no kind: `relate`
         * permits an unkinded edge, and an absent field says "no label" without
         * inviting a client to match on the empty string. */
        if (h->via_kind) {
            cJSON_AddStringToObject(o, "via_kind", h->via_kind);
        } else if (h->via_kind_uncertain) {
            /* An absent via_kind means "this edge has no kind". This edge has
             * one; the reverse index just could not intern it, so it cannot say
             * what it is — and under a kind filter this hop is a candidate it
             * could not rule out. Saying so beats letting it read as unkinded. */
            cJSON_AddBoolToObject(o, "via_kind_unknown", 1);
        }
        /* Which way the edge was walked. Always reported, not just under
         * `direction: both`, so a client never has to remember what it asked
         * for to read the answer. */
        cJSON_AddStringToObject(o, "via_direction",
                                h->via_incoming ? "in" : "out");
    }
    return o;
}

static cJSON *handle_traverse(AegisDB *db, const cJSON *req,
                              const AuthCtx *ctx) {
    const char *ns = ctx->ns;
    uint64_t id;
    if (jr_u64(req, "id", &id) != 0) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    uint64_t depth = 1;
    jr_u64(req, "depth", &depth);
    /* Clamp before the cast to int: an untrusted u64 depth would otherwise cast
     * to a garbage (possibly negative) value, and a huge depth is pathological
     * work regardless. A graph walk deeper than this isn't useful. */
    if (depth > MAX_TRAVERSE_DEPTH) {
        depth = MAX_TRAVERSE_DEPTH;
    }
    /* Rejected rather than ignored on an unknown value: silently walking the
     * wrong way is worse than an error. `in`/`both` need the reverse index, and
     * qe_traverse_ex reports NOT_READY when --no-edge-index disabled it. */
    const char *dir = jr_str(req, "direction", "out");
    TraverseDirection tdir;
    if (strcmp(dir, "out") == 0) {
        tdir = TRAVERSE_OUT;
    } else if (strcmp(dir, "in") == 0) {
        tdir = TRAVERSE_IN;
    } else if (strcmp(dir, "both") == 0) {
        tdir = TRAVERSE_BOTH;
    } else {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    const char **kinds = NULL;
    size_t kn = 0;
    if (jr_str_array(req, "kinds", &kinds, &kn, MAX_TRAVERSE_KINDS) != 0) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    /* jr_str_array skips non-string elements and treats a non-array as absent,
     * both of which land on kn == 0 — which every filter downstream reads as
     * "follow every kind". A caller that asked to narrow would silently get the
     * widest possible walk, which is the same failure `direction` is strict
     * about just above. So require the array to be an array and to have parsed
     * whole. */
    const cJSON *karr = cJSON_GetObjectItemCaseSensitive(req, "kinds");
    if (karr && !cJSON_IsNull(karr) &&
        (!cJSON_IsArray(karr) || (size_t)cJSON_GetArraySize(karr) != kn)) {
        free(kinds);
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }

    TraverseParams p;
    memset(&p, 0, sizeof(p));
    p.start_id = id;
    p.depth = (int)depth;
    p.agent_filter = ns ? ns : jr_str(req, "agent_id", NULL);
    p.direction = tdir;
    p.kinds = kinds;
    p.kind_count = kn;

    MemoryRecord *recs = NULL;
    TraverseHop *hops = NULL;
    size_t n = 0;
    int capped = 0;
    aegis_status_t st = qe_traverse_ex(db, &p, &recs, &hops, &n, &capped);
    free(kinds);
    if (st != AEGIS_OK) {
        return json_error_status(st);
    }
    int emb = want_embeddings(req);
    cJSON *o = json_ok();
    cJSON *arr = cJSON_AddArrayToObject(o, "records");
    for (size_t i = 0; i < n; i++) {
        cJSON *jr = json_record(&recs[i], emb);
        if (hops) {
            cJSON_AddItemToObject(jr, "traversal", traversal_json(&hops[i]));
        }
        cJSON_AddItemToArray(arr, jr);
        record_free(&recs[i]);
    }
    free(recs);
    traverse_hops_free(hops, n);
    cJSON_AddNumberToObject(o, "total", (double)n);
    /* Same signal `count` uses: the walk hit its node ceiling, so `records` is a
     * prefix of the reachable set rather than all of it. Reported only when it
     * happens, so a normal response is unchanged. */
    if (capped) {
        cJSON_AddBoolToObject(o, "capped", 1);
    }
    return o;
}

/* Constant-time string equality: no early exit on mismatch, and length
 * differences fold into the result so timing does not leak the secret. */
/* Constant-time fixed-length byte compare (no early exit). */
static int ct_eq_bytes(const uint8_t *a, const uint8_t *b, size_t n) {
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static uint64_t
monotonic_micros(void); /* defined below; used by the rate limiter */

/* ----- runtime token administration (admin-only ops) ------------------- */

static int scope_from_str(const char *s, int *out) {
    if (!s || strcmp(s, "rw") == 0) {
        *out = AEGIS_SCOPE_RW;
        return 0;
    }
    if (strcmp(s, "ro") == 0) {
        *out = AEGIS_SCOPE_RO;
        return 0;
    }
    if (strcmp(s, "admin") == 0) {
        *out = AEGIS_SCOPE_ADMIN;
        return 0;
    }
    return -1;
}
static const char *scope_str(int scope) {
    return scope == AEGIS_SCOPE_RO      ? "ro"
           : scope == AEGIS_SCOPE_ADMIN ? "admin"
                                        : "rw";
}

/* Mint a random 256-bit token as 64 hex chars (out must hold 65). 0/-1. */
static int gen_random_token(char out[65]) {
    uint8_t buf[32];
    FILE *f = fopen("/dev/urandom", "rbe");
    if (!f) {
        return -1;
    }
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n != sizeof buf) {
        return -1;
    }
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = hx[buf[i] >> 4];
        out[(2 * i) + 1] = hx[buf[i] & 0xf];
    }
    out[64] = '\0';
    return 0;
}

/* token_list: fingerprint + namespace + scope of every token (no secrets). */
static cJSON *handle_token_list(AegisDB *db, const cJSON *req,
                                const AuthCtx *ctx) {
    (void)req;
    (void)ctx;
    cJSON *o = json_ok();
    cJSON *arr = cJSON_AddArrayToObject(o, "tokens");
    pthread_rwlock_rdlock(&db->auth_lock);
    for (size_t i = 0; arr && i < db->config.auth_token_count; i++) {
        const AuthToken *t = &db->config.auth_tokens[i];
        char id[13];
        config_token_fingerprint(t, id);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", id);
        if (t->namespace) {
            cJSON_AddStringToObject(e, "namespace", t->namespace);
        }
        cJSON_AddStringToObject(e, "scope", scope_str(t->scope));
        cJSON_AddItemToArray(arr, e);
    }
    pthread_rwlock_unlock(&db->auth_lock);
    return o;
}

/* A token-file namespace is written into a space- and newline-delimited line
 * (config_write_token_file: "sha256$<hex> <ns> <scope>"), so it must contain no
 * whitespace or control characters. Otherwise a crafted namespace could inject
 * additional token-file lines — e.g. an embedded newline followed by a bare
 * token, which parses as a global-admin token — that survive a reload. Stricter
 * than valid_agent_id (which permits spaces) because of the delimited format. */
static int token_namespace_ok(const char *ns) {
    if (!ns) {
        return 0;
    }
    size_t n = strlen(ns);
    if (n < 1 || n > MAX_AGENT_ID) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)ns[i];
        if (c <= 0x20 || c == 0x7f) {
            return 0; /* printable, no whitespace/control */
        }
    }
    return 1;
}

/* token_add: add a token (generating a secret if none supplied) and persist to
 * the token file. Returns its fingerprint (and the plaintext, once, if minted). */
static cJSON *handle_token_add(AegisDB *db, const cJSON *req,
                               const AuthCtx *ctx) {
    (void)ctx;
    /* The secret to add is "new_token" (the request's "token" is the caller's
     * own auth credential); omit it to have the server mint one. */
    const char *tok = jr_str(req, "new_token", NULL);
    const char *ns = jr_str(req, "namespace", NULL);
    int scope;
    if (scope_from_str(jr_str(req, "scope", ns ? "rw" : "admin"), &scope) !=
        0) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    /* admin scope is global (no namespace); a namespaced token is ro/rw. */
    if (scope == AEGIS_SCOPE_ADMIN) {
        ns = NULL;
    } else if (!token_namespace_ok(ns)) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }

    char generated[65];
    int minted = 0;
    if (!tok) {
        if (gen_random_token(generated) != 0) {
            return json_error_status(AEGIS_ERR_INTERNAL);
        }
        tok = generated;
        minted = 1;
    }

    pthread_rwlock_wrlock(&db->auth_lock);
    if (config_add_token(&db->config, tok, ns, scope) != 0) {
        pthread_rwlock_unlock(&db->auth_lock);
        return json_error_status(AEGIS_ERR_INTERNAL);
    }
    char id[13];
    config_token_fingerprint(
        &db->config.auth_tokens[db->config.auth_token_count - 1], id);
    int persisted = 0;
    if (db->config.auth_token_file[0]) {
        persisted = (config_write_token_file(&db->config,
                                             db->config.auth_token_file) == 0);
    }
    pthread_rwlock_unlock(&db->auth_lock);
    if (db->config.auth_token_file[0] && !persisted) {
        LOG_WARN("token_add: could not persist to %s",
                 db->config.auth_token_file);
    }

    cJSON *o = json_ok();
    cJSON_AddStringToObject(o, "id", id);
    if (minted) {
        cJSON_AddStringToObject(o, "token", generated); /* shown once */
    }
    cJSON_AddBoolToObject(o, "persisted", persisted);
    return o;
}

/* token_revoke: remove the token with the given fingerprint id and persist. */
static cJSON *handle_token_revoke(AegisDB *db, const cJSON *req,
                                  const AuthCtx *ctx) {
    (void)ctx;
    const char *id = jr_str(req, "id", NULL);
    if (!id) {
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    pthread_rwlock_wrlock(&db->auth_lock);
    int removed = config_remove_token(&db->config, id);
    int persisted = 0;
    if (removed && db->config.auth_token_file[0]) {
        persisted = (config_write_token_file(&db->config,
                                             db->config.auth_token_file) == 0);
    }
    pthread_rwlock_unlock(&db->auth_lock);
    if (!removed) {
        return json_error_status(AEGIS_ERR_NOT_FOUND);
    }
    cJSON *o = json_ok();
    cJSON_AddBoolToObject(o, "revoked", 1);
    cJSON_AddBoolToObject(o, "persisted", persisted);
    return o;
}

/* Authenticate a request and resolve the caller's namespace + scope. Auth is
 * disabled (unrestricted) when no tokens are configured. Every token is checked
 * without an early break so timing does not reveal which token matched. Returns
 * AEGIS_OK (ctx filled) or AEGIS_ERR_UNAUTHORIZED. */
static aegis_status_t resolve_identity(AegisDB *db, const cJSON *req,
                                       AuthCtx *out) {
    out->ns_buf[0] = '\0';
    out->ns = NULL;
    out->can_write = 1;
    /* auth_lock guards the token set against concurrent token_add/revoke. */
    pthread_rwlock_rdlock(&db->auth_lock);
    if (db->config.auth_token_count == 0) {
        pthread_rwlock_unlock(&db->auth_lock);
        return AEGIS_OK; /* auth disabled */
    }

    const char *token = jr_str(req, "token", NULL);
    const AuthToken *match = NULL;
    if (token) {
        /* Compare SHA-256 digests, never the raw token: a plaintext-string
         * compare iterates for max(len) and so leaks the stored token's length
         * via timing. Hashing both sides makes every comparison a fixed 32-byte
         * constant-time check regardless of token length or storage form. */
        uint8_t th[SHA256_DIGEST_LEN];
        sha256(token, strlen(token), th);
        for (size_t i = 0; i < db->config.auth_token_count; i++) {
            const AuthToken *t = &db->config.auth_tokens[i];
            uint8_t cand[SHA256_DIGEST_LEN];
            const uint8_t *want;
            if (t->hashed) {
                want = t->hash;
            } else {
                sha256(t->token, strlen(t->token), cand);
                want = cand;
            }
            if (ct_eq_bytes(th, want, SHA256_DIGEST_LEN)) {
                match = t;
            }
        }
    }
    aegis_status_t st = AEGIS_ERR_UNAUTHORIZED;
    if (match) {
        if (match->namespace) { /* copy so it survives past the lock */
            snprintf(out->ns_buf, sizeof(out->ns_buf), "%s", match->namespace);
            out->ns = out->ns_buf;
        }
        out->can_write = (match->scope != AEGIS_SCOPE_RO);
        st = AEGIS_OK;
    }
    pthread_rwlock_unlock(&db->auth_lock);
    return st;
}

typedef cJSON *(*OpHandler)(AegisDB *db, const cJSON *req, const AuthCtx *ctx);

/* Per-operation flags. */
enum {
    OP_WRITE =
        1U, /* mutates: needs a write token AND refused on a read-only replica */
    OP_GLOBAL =
        2U, /* admin/global token only — a namespaced caller is forbidden */
    OP_WRITE_TOK =
        4U, /* needs a write token, but is NOT a replica write (snapshot) */
    OP_NOAUTH = 8U, /* dispatched before identity resolution (ping) */
};

typedef struct {
    const char *name;
    OpHandler fn;
    unsigned flags;
    MetricOp metric;
} OpDef;

/* The single source of truth for the operation set: dispatch, the auth/write
 * policy, and the per-op metric are all driven from this one table (previously
 * duplicated across dispatch_inner, is_write_op, metric_op, and op_names). */
static const OpDef OPS[] = {
    {"ping", handle_ping, OP_NOAUTH, MOP_PING},
    {"stats", handle_stats, OP_GLOBAL, MOP_STATS},
    {"token_list", handle_token_list, OP_GLOBAL, MOP_OTHER},
    {"token_add", handle_token_add, OP_GLOBAL, MOP_OTHER},
    {"token_revoke", handle_token_revoke, OP_GLOBAL, MOP_OTHER},
    {"snapshot", handle_snapshot, OP_GLOBAL | OP_WRITE_TOK, MOP_OTHER},
    {"insert", handle_insert, OP_WRITE, MOP_INSERT},
    {"get", handle_get, 0, MOP_GET},
    {"history", handle_history, 0, MOP_HISTORY},
    {"update", handle_update, OP_WRITE, MOP_UPDATE},
    {"delete", handle_delete, OP_WRITE, MOP_DELETE},
    {"export", handle_export, 0, MOP_EXPORT},
    {"purge", handle_purge, OP_WRITE, MOP_PURGE},
    {"search", handle_search, 0, MOP_SEARCH},
    {"count", handle_count, 0, MOP_COUNT},
    {"consolidate", handle_consolidate, OP_WRITE, MOP_CONSOLIDATE},
    {"forget", handle_forget, OP_WRITE, MOP_FORGET},
    {"promote", handle_promote, OP_WRITE, MOP_PROMOTE},
    {"relate", handle_relate, OP_WRITE, MOP_RELATE},
    {"traverse", handle_traverse, 0, MOP_TRAVERSE},
};

static const OpDef *find_op(const char *op) {
    if (!op) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) {
        if (strcmp(op, OPS[i].name) == 0) {
            return &OPS[i];
        }
    }
    return NULL;
}

/* Copy `op` into `dst` for safe logging: printable ASCII only (anything else
 * becomes '?'), truncated to fit. The operation string is unauthenticated
 * client input and is logged before auth, so logging it raw would let a client
 * inject newlines / terminal escapes into an operator's console. */
static void sanitize_for_log(char *dst, size_t dstsz, const char *op) {
    size_t i = 0;
    for (; op[i] && i + 1 < dstsz; i++) {
        unsigned char c = (unsigned char)op[i];
        dst[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    dst[i] = '\0';
}

static cJSON *dispatch_inner(AegisDB *db, const cJSON *req) {
    const char *op = jr_str(req, "operation", NULL);
    if (!op) {
        LOG_DEBUG("dispatch: request with no \"operation\" field");
        return json_error_status(AEGIS_ERR_INVALID_REQUEST);
    }
    char opsafe[64];
    sanitize_for_log(opsafe, sizeof opsafe, op); /* op is untrusted; log this */
    const OpDef *d = find_op(op);
    if (!d) {
        LOG_WARN("dispatch: unknown operation \"%s\"", opsafe);
        return json_error("INVALID_REQUEST", "unknown operation");
    }

    /* "ping" is exempt so liveness and startup probes work unauthenticated. */
    if (d->flags & OP_NOAUTH) {
        return d->fn(db, req, NULL);
    }

    AuthCtx ctx;
    aegis_status_t ast = resolve_identity(db, req, &ctx);
    if (ast != AEGIS_OK) {
        LOG_WARN("dispatch: unauthorized \"%s\" request rejected", opsafe);
        return json_error_status(ast);
    }
    if ((d->flags & OP_WRITE) && !ctx.can_write) {
        LOG_WARN("dispatch: read-only token attempted \"%s\"", opsafe);
        return json_error_status(AEGIS_ERR_FORBIDDEN);
    }
    /* A read-only replica serves reads from its followed copy but never accepts
     * writes — direct those at the primary. */
    if (db->config.read_only && (d->flags & OP_WRITE)) {
        LOG_DEBUG("dispatch: write \"%s\" refused on read-only replica",
                  opsafe);
        return json_error_status(AEGIS_ERR_READ_ONLY);
    }

    /* Per-tenant rate limit: bound a single namespace's request rate so one
     * runaway agent can't monopolize the shared server. Namespaced callers only
     * (admin/unrestricted are exempt); ping already returned above. */
    if (ctx.ns && db->config.tenant_rate_qps > 0 &&
        !tenant_rate_allow(db->tenants, ctx.ns, db->config.tenant_rate_qps,
                           db->config.tenant_rate_qps, monotonic_micros())) {
        LOG_WARN("dispatch: rate limit hit for ns=%s on \"%s\"", ctx.ns,
                 opsafe);
        return json_error_status(AEGIS_ERR_RATE_LIMITED);
    }

    /* Admin-only ops (stats, token_*, snapshot) forbid a namespaced caller;
     * snapshot additionally requires a write token. */
    if ((d->flags & OP_GLOBAL) && ctx.ns) {
        return json_error_status(AEGIS_ERR_FORBIDDEN);
    }
    if ((d->flags & OP_WRITE_TOK) && !ctx.can_write) {
        return json_error_status(AEGIS_ERR_FORBIDDEN);
    }

    LOG_DEBUG("dispatch: operation \"%s\" (ns=%s)", opsafe,
              ctx.ns ? ctx.ns : "*");
    return d->fn(db, req, &ctx);
}

static MetricOp metric_op(const char *op) {
    const OpDef *d = find_op(op);
    return d ? d->metric : MOP_OTHER;
}

/* Finite bucket bounds in microseconds: 100µs … 250ms. Anything slower lands in
 * the overflow bucket. Declared in db.h. */
const uint64_t recall_hist_bounds[RECALL_HIST_N - 1] = {
    100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000, 250000,
};

/* Record one recall observation. Linear scan over 11 bounds — cheaper than the
 * branch misprediction of anything cleverer, and it runs once per search. */
static void recall_observe(Metrics *m, uint64_t micros) {
    size_t b = RECALL_HIST_N - 1; /* overflow unless a bound claims it */
    for (size_t i = 0; i < RECALL_HIST_N - 1; i++) {
        if (micros <= recall_hist_bounds[i]) {
            b = i;
            break;
        }
    }
    atomic_fetch_add_explicit(&m->recall_hist[b], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&m->recall_micros, micros, memory_order_relaxed);
    atomic_fetch_add_explicit(&m->recall_count, 1, memory_order_relaxed);
}

static uint64_t monotonic_micros(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000ULL) +
           ((uint64_t)ts.tv_nsec / 1000ULL);
}

/* True if `resp` is an error response, and if so its error code (or NULL). */
static int resp_is_error(const cJSON *resp, const char **code) {
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(resp, "ok");
    if (!ok || !cJSON_IsFalse(ok)) {
        return 0;
    }
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    const cJSON *c = err ? cJSON_GetObjectItemCaseSensitive(err, "code") : NULL;
    if (code) {
        *code = (c && cJSON_IsString(c)) ? c->valuestring : NULL;
    }
    return 1;
}

/* Public entry: dispatch and record operational metrics (all lock-free atomic,
 * memory_order_relaxed — counters need eventual correctness, not ordering). */
cJSON *query_engine_dispatch(AegisDB *db, const cJSON *req) {
    uint64_t t0 = monotonic_micros();
    cJSON *resp = dispatch_inner(db, req);
    uint64_t elapsed = monotonic_micros() - t0;

    Metrics *m = &db->metrics;
    MetricOp op = metric_op(jr_str(req, "operation", NULL));
    atomic_fetch_add_explicit(&m->requests, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&m->by_op[op], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&m->dispatch_micros, elapsed,
                              memory_order_relaxed);
    /* `search` is the recall path — the one in the agent's inner loop, and the
     * only one whose tail latency is worth a histogram. */
    if (op == MOP_SEARCH) {
        recall_observe(m, elapsed);
    }
    const char *code = NULL;
    if (resp && resp_is_error(resp, &code)) {
        atomic_fetch_add_explicit(&m->errors, 1, memory_order_relaxed);
        if (code && strcmp(code, "UNAUTHORIZED") == 0) {
            atomic_fetch_add_explicit(&m->unauthorized, 1,
                                      memory_order_relaxed);
        }
    }
    return resp;
}
