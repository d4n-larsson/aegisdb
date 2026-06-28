/* AegisDB server configuration (T008). */
#ifndef AEGISDB_CONFIG_H
#define AEGISDB_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define AEGIS_VERSION_STRING "0.1.0"

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

typedef struct {
    int listen_port;            /* default 9470 */
    char data_dir[1024];        /* default "./data" */
    size_t max_payload_bytes;   /* default 1 MiB */
    size_t embedding_dimensions;/* default 384 */
    uint32_t working_capacity;  /* default 256 */
    uint64_t default_ttl_ms;    /* default 3600000 */
    size_t fsync_batch_size;    /* default 1000; used in BATCH mode */
    int durability;             /* AegisDurability; default INTERVAL */
    uint64_t fsync_interval_ms; /* INTERVAL flush cadence; default 1000 */
    unsigned checkpoint_sec;    /* index checkpoint cadence; 0 disables; def 60 */
    int worker_threads;         /* default 4 */
    int enabled_phase;          /* default 4: gate operations above this phase */
    int run_health_check;       /* 1 if --health-check: probe a server and exit */
    int log_level;              /* AegisLogLevel; default AEGIS_LOG_INFO (2) */

    /* Accepted bearer tokens. When auth_token_count == 0 authentication is
     * disabled and every request is served. Otherwise each request (except
     * "ping") must carry a "token" matching one of these. Owned by the Config;
     * release with config_free(). */
    char **auth_tokens;
    size_t auth_token_count;
} Config;

/* Populate cfg with documented defaults. */
void config_defaults(Config *cfg);

/* Human-readable name for a durability mode ("sync"|"batch"|"interval"). */
const char *aegis_durability_name(int mode);

/* Parse a durability mode name into *out. Returns 0 on success, -1 if the
 * string is not a recognized mode. */
int aegis_durability_from_string(const char *s, int *out);

/* The effective per-write fsync count threshold for cfg's durability mode:
 * 1 for SYNC, fsync_batch_size for BATCH, SIZE_MAX (never) for INTERVAL. */
size_t config_effective_fsync_batch(const Config *cfg);

/* Parse argv (supports --data-dir, --port, --phase, --workers,
 * --max-payload, --embedding-dim, --fsync-batch, --working-capacity,
 * --auth-token, --auth-token-file, --log-level, --help).
 * Returns 0 on success, -1 on error, 1 if --help was requested. */
int config_parse_args(Config *cfg, int argc, char **argv);

/* Free heap-owned config fields (the auth token list). Safe to call once on a
 * Config populated by config_defaults/config_parse_args. */
void config_free(Config *cfg);

#endif /* AEGISDB_CONFIG_H */