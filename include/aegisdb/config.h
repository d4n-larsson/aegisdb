/* AegisDB server configuration (T008). */
#ifndef AEGISDB_CONFIG_H
#define AEGISDB_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* Baked in at build time from the git tag (-DAEGIS_VERSION_STRING=...); the
 * build systems derive it from `git describe`, and the release image/CI pass the
 * exact tag. Falls back to a dev marker for ad-hoc builds. */
#ifndef AEGIS_VERSION_STRING
#define AEGIS_VERSION_STRING "0.0.0-dev"
#endif

/* Write durability policy: how aggressively the log is fsync'd to disk.
 *   SYNC     - fsync before acknowledging every write (no acknowledged loss).
 *   BATCH    - fsync once per fsync_batch_size writes (bounds loss by count,
 *              but an idle server may leave the last batch unflushed forever).
 *   INTERVAL - fsync at most every fsync_interval_ms via the maintenance
 *              thread (bounds loss by time regardless of write rate). */
typedef enum {
    AEGIS_DURABILITY_SYNC = 0,
    AEGIS_DURABILITY_BATCH,
    AEGIS_DURABILITY_INTERVAL,
} AegisDurability;

/* Authorization scope for a token.
 *   RO    - read-only within its namespace.
 *   RW    - read+write within its namespace.
 *   ADMIN - unrestricted: any namespace, all operations (a global token). */
typedef enum {
    AEGIS_SCOPE_RO = 0,
    AEGIS_SCOPE_RW,
    AEGIS_SCOPE_ADMIN,
} AegisScope;

/* An accepted bearer token and the tenant/scope it is bound to. The credential
 * is stored either as plaintext (`token`) or, when configured with a
 * `sha256$<hex>` entry, as the digest (`hash`); `hashed` selects which. */
typedef struct {
    char *token;                  /* plaintext secret, owned; NULL when hashed */
    uint8_t hash[32];             /* sha256(token) when hashed */
    int hashed;                   /* 1: compare against `hash`; 0: against `token` */
    char *namespace;              /* tenant (agent_id); owned; NULL for ADMIN */
    int scope;                    /* AegisScope */
} AuthToken;

typedef struct {
    int listen_port;            /* default 9470 */
    char data_dir[1024];        /* default "./data" */
    size_t max_payload_bytes;   /* default 1 MiB */
    size_t embedding_dimensions;/* default 384 */
    size_t ann_threshold;       /* live-vector count above which semantic search switches to HNSW; 0 = built-in default */
    size_t ann_ef_search;       /* HNSW query beam width once the semantic index is large; 0 = HNSW default. Recall/latency knob */
    int ann_quantize;           /* 1: store HNSW vectors as int8 (~4x smaller, small recall cost); default 0 */
    size_t ann_shard_target;    /* target vectors per HNSW shard; the graph splits into ~n/target shards (capped by CPUs) so the build parallelizes. 0 = built-in default */
    /* Per-tenant (namespace) limits for a shared multi-tenant server; enforced
     * only when auth is enabled. 0 = unlimited. */
    size_t tenant_max_records;  /* max live records per namespace */
    size_t tenant_max_bytes;    /* max live bytes (frame payloads) per namespace */
    double tenant_rate_qps;     /* max sustained requests/sec per namespace (burst = 1s worth) */
    uint32_t working_capacity;  /* default 256 */
    uint64_t default_ttl_ms;    /* default 3600000 */
    size_t fsync_batch_size;    /* default 1000; used in BATCH mode */
    int durability;             /* AegisDurability; default INTERVAL */
    uint64_t fsync_interval_ms; /* INTERVAL flush cadence; default 1000 */
    unsigned checkpoint_sec;    /* index checkpoint cadence; 0 disables; def 60 */
    unsigned compact_sec;       /* compaction check cadence (runs only when >=25% dead); 0 disables; def 300 */
    int io_threads;             /* poll() event-loop threads (dispatch parallelism, NOT a connection cap); default 2x CPUs (8-64) */
    /* DoS guards on the client port. idle_timeout_sec reaps a connection that
     * has moved no bytes for that long (defeats slow-loris / stalled sockets);
     * max_connections is a hard cap on concurrent client connections (total
     * across io-threads). 0 = disabled for each. */
    unsigned idle_timeout_sec;  /* default 60; 0 disables idle reaping */
    int max_connections;        /* default 0 (unlimited) */
    /* Bounds a single broad (filterless / whole-time-range) search or count:
     * at most this many of the most-recent matching records are loaded into
     * memory, so an unfiltered query cannot pull the entire dataset into RAM
     * (amplification DoS). Selective filters (tags) are unaffected. Default
     * 100000; 0 = unlimited. */
    size_t query_scan_cap;
    int enabled_phase;          /* default 4: gate operations above this phase */
    /* Replication (Phase 1 read replicas). A primary opens a replication source
     * on `replication_port` when a token is set; a replica sets
     * `replicate_from_host`/`_port` to follow one. `read_only` refuses client
     * writes (implied on a replica). See docs/read-replica-design.md. */
    int replication_port;         /* >0: serve the replication stream on this port */
    char replication_token[128];  /* required to subscribe / sent when subscribing */
    char replicate_from_host[256];/* non-empty: this node is a read-only replica */
    int replicate_from_port;      /* primary's replication port */
    int read_only;                /* 1: refuse client writes (implied by replicate_from) */
    int run_health_check;       /* 1 if --health-check: probe a server and exit */
    const char *hash_token;     /* --hash-token <tok>: print sha256$<hex> & exit */
    const char *restore_from;   /* --restore <dir>: install a snapshot into --data-dir & exit */
    int log_level;              /* AegisLogLevel; default AEGIS_LOG_INFO (2) */

    /* Accepted bearer tokens. When auth_token_count == 0 authentication is
     * disabled and every request is served with unrestricted access. Otherwise
     * each request (except "ping") must carry a "token" matching one of these;
     * the matched token's namespace and scope then constrain the request. Owned
     * by the Config; release with config_free(). */
    AuthToken *auth_tokens;
    size_t auth_token_count;
    /* Path passed to --auth-token-file, retained so runtime token-admin ops
     * (token_add/token_revoke) can persist changes back to it. Empty when auth
     * was configured without a file (runtime changes then cannot persist). */
    char auth_token_file[1024];
} Config;

/* Populate cfg with documented defaults. */
void config_defaults(Config *cfg);

/* ----- runtime token administration (token_add/token_revoke/token_list) ----
 * These mutate cfg->auth_tokens in place; the caller serializes them against
 * token readers (the server holds db->auth_lock for write around these and for
 * read around matching). */

/* Add a token bound to `ns` (NULL = global admin) with `scope`. `tok` is a
 * plaintext secret or a `sha256$<hex>` digest. Returns 0/-1 (OOM/malformed). */
int config_add_token(Config *cfg, const char *tok, const char *ns, int scope);

/* A stable public fingerprint of a token: the first 12 hex chars of its
 * SHA-256 (of the plaintext, or the stored digest for hashed entries). Writes
 * 13 bytes (12 + NUL) to `out`. Lets token_list/revoke reference a token
 * without exposing the secret. */
void config_token_fingerprint(const AuthToken *t, char out[13]);

/* Remove the token whose fingerprint equals `id12`. Returns 1 if removed, 0 if
 * no match. */
int config_remove_token(Config *cfg, const char *id12);

/* Rewrite `path` with the current token set, one `sha256$<hex> [ns] [scope]`
 * line each (all stored hashed), atomically and 0600. Returns 0/-1. */
int config_write_token_file(const Config *cfg, const char *path);

/* Human-readable name for a durability mode ("sync"|"batch"|"interval"). */
const char *aegis_durability_name(int mode);

/* Parse a durability mode name into *out. Returns 0 on success, -1 if the
 * string is not a recognized mode. */
int aegis_durability_from_string(const char *s, int *out);

/* The effective per-write fsync count threshold for cfg's durability mode:
 * 1 for SYNC, fsync_batch_size for BATCH, SIZE_MAX (never) for INTERVAL. */
size_t config_effective_fsync_batch(const Config *cfg);

/* Parse argv (supports --data-dir, --port, --phase, --io-threads (alias
 * --workers), --max-payload, --embedding-dim, --ann-ef-search,
 * --ann-threshold, --ann-quantize, --ann-shard-target, --tenant-max-records,
 * --tenant-max-bytes, --tenant-rate-qps, --fsync-batch,
 * --working-capacity, --auth-token, --auth-token-file, --log-level, --help).
 * Returns 0 on success, -1 on error, 1 if --help was requested. */
int config_parse_args(Config *cfg, int argc, char **argv);

/* Free heap-owned config fields (the auth token list). Safe to call once on a
 * Config populated by config_defaults/config_parse_args. */
void config_free(Config *cfg);

#endif /* AEGISDB_CONFIG_H */