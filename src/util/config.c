/* Server configuration defaults and CLI parsing (T008). */
#include "aegisdb/config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aegisdb/logging.h"
#include "aegisdb/sha256.h"

/* Default count of poll() event-loop (I/O) threads. Connections are sharded
 * across these threads and a connection's request is dispatched inline, so this
 * sets dispatch parallelism — it does NOT cap concurrent connections. Scale
 * with the machine, with a floor and a cap so huge boxes don't over-spawn. */
static int default_io_threads(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    long want = (n > 0) ? n * 2 : 8;
    if (want < 8) want = 8;
    if (want > 64) want = 64;
    return (int)want;
}

void config_defaults(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->listen_port = 9470;
    strncpy(cfg->data_dir, "./data", sizeof(cfg->data_dir) - 1);
    cfg->max_payload_bytes = 1048576; /* 1 MiB */
    cfg->embedding_dimensions = 384;
    cfg->ann_threshold = 0; /* 0 -> built-in default */
    cfg->ann_ef_search = 0; /* 0 -> HNSW built-in default */
    cfg->ann_quantize = 0;  /* float32 vectors by default */
    cfg->ann_shard_target = 0; /* 0 -> built-in default */
    cfg->tenant_max_records = 0; /* 0 -> unlimited */
    cfg->tenant_max_bytes = 0;   /* 0 -> unlimited */
    cfg->tenant_rate_qps = 0;    /* 0 -> unlimited */
    cfg->replication_port = 0;   /* 0 -> replication source disabled */
    cfg->replication_token[0] = '\0';
    cfg->replicate_from_host[0] = '\0'; /* empty -> not a replica */
    cfg->replicate_from_port = 0;
    cfg->read_only = 0;
    cfg->working_capacity = 256;
    cfg->default_ttl_ms = 3600000; /* 1 hour */
    cfg->fsync_batch_size = 1000;
    cfg->durability = AEGIS_DURABILITY_INTERVAL;
    cfg->fsync_interval_ms = 1000;
    cfg->checkpoint_sec = 60;
    cfg->compact_sec = 300;
    cfg->io_threads = default_io_threads();
    cfg->idle_timeout_sec = 60; /* reap connections idle (no byte progress) 60s */
    cfg->max_connections = 0;   /* 0 = unlimited */
    cfg->query_scan_cap = 100000; /* cap broad/filterless search+count loads */
    cfg->max_index_bytes = 0;     /* 0 = unlimited index RAM */
    cfg->enabled_phase = 4; /* all features enabled by default */
    cfg->log_level = AEGIS_LOG_INFO;

    /* AEGISDB_LOG_LEVEL seeds the default; --log-level overrides it. */
    const char *env = getenv("AEGISDB_LOG_LEVEL");
    if (env && *env) {
        AegisLogLevel lvl;
        if (aegis_log_level_from_string(env, &lvl) == 0)
            cfg->log_level = lvl;
    }
}

const char *aegis_durability_name(int mode) {
    switch (mode) {
        case AEGIS_DURABILITY_SYNC: return "sync";
        case AEGIS_DURABILITY_BATCH: return "batch";
        case AEGIS_DURABILITY_INTERVAL: return "interval";
        default: return "unknown";
    }
}

int aegis_durability_from_string(const char *s, int *out) {
    if (!s) return -1;
    if (strcmp(s, "sync") == 0) *out = AEGIS_DURABILITY_SYNC;
    else if (strcmp(s, "batch") == 0) *out = AEGIS_DURABILITY_BATCH;
    else if (strcmp(s, "interval") == 0) *out = AEGIS_DURABILITY_INTERVAL;
    else return -1;
    return 0;
}

size_t config_effective_fsync_batch(const Config *cfg) {
    switch (cfg->durability) {
        case AEGIS_DURABILITY_SYNC: return 1;
        case AEGIS_DURABILITY_INTERVAL: return SIZE_MAX; /* never on count */
        default: return cfg->fsync_batch_size; /* BATCH */
    }
}

/* Decode `n` hex chars from `s` into `out`. Returns 0 on success, -1 on a
 * non-hex digit or odd length. */
static int hex_decode(const char *s, uint8_t *out, size_t n) {
    if (n % 2) return -1;
    for (size_t i = 0; i < n; i += 2) {
        int hi = -1, lo = -1;
        char a = s[i], b = s[i + 1];
        hi = (a >= '0' && a <= '9') ? a - '0'
             : (a >= 'a' && a <= 'f') ? a - 'a' + 10
             : (a >= 'A' && a <= 'F') ? a - 'A' + 10 : -1;
        lo = (b >= '0' && b <= '9') ? b - '0'
             : (b >= 'a' && b <= 'f') ? b - 'a' + 10
             : (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
        if (hi < 0 || lo < 0) return -1;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* Load a 32-byte encryption key from `path`: exactly 64 hex chars on the first
 * line (trailing whitespace/newline ignored). Returns 0 on success, -1 on an
 * unreadable file or a malformed key. Never logs the key. */
static int load_key_file(const char *path, uint8_t out[AEAD_KEY_LEN]) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    char *got = fgets(line, sizeof line, f);
    fclose(f);
    if (!got) return -1;
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                     line[n - 1] == ' ' || line[n - 1] == '\t'))
        line[--n] = '\0';
    if (n != 2 * AEAD_KEY_LEN) {
        explicit_bzero(line, sizeof(line));
        return -1;
    }
    int rc = hex_decode(line, out, 2 * AEAD_KEY_LEN);
    explicit_bzero(line, sizeof(line)); /* don't leave the raw key on the stack */
    return rc;
}

/* Append a token bound to `ns` (NULL = global) with `scope`. `tok` is either a
 * plaintext secret or a `sha256$<64-hex>` digest (hashed at rest). Copies the
 * strings. Returns 0 on success, -1 on OOM or a malformed hash. */
static int append_token(Config *cfg, const char *tok, const char *ns,
                        int scope) {
    AuthToken *grown = realloc(cfg->auth_tokens,
                              (cfg->auth_token_count + 1) * sizeof(AuthToken));
    if (!grown) return -1;
    cfg->auth_tokens = grown;
    AuthToken *t = &cfg->auth_tokens[cfg->auth_token_count];
    t->token = NULL;
    t->namespace = NULL;
    t->hashed = 0;
    memset(t->hash, 0, sizeof(t->hash));

    if (strncmp(tok, "sha256$", 7) == 0) {
        const char *hex = tok + 7;
        if (strlen(hex) != 64 || hex_decode(hex, t->hash, 64) != 0) return -1;
        t->hashed = 1;
    } else {
        t->token = strdup(tok);
        if (!t->token) return -1;
    }
    if (ns) {
        t->namespace = strdup(ns);
        if (!t->namespace) {
            free(t->token);
            return -1;
        }
    }
    t->scope = scope;
    cfg->auth_token_count++;
    return 0;
}

/* Advance past the current whitespace-delimited field, NUL-terminating it, and
 * return a pointer to the next field (or NULL if none). */
static char *next_field(char *s) {
    while (*s && *s != ' ' && *s != '\t') s++;
    if (!*s) return NULL;
    *s++ = '\0';
    while (*s == ' ' || *s == '\t') s++;
    return *s ? s : NULL;
}

/* Parse one token-file line (already trimmed) into the token list. Format:
 *   <token>                       -> global admin token (any namespace)
 *   <token> <namespace>           -> namespaced, read+write
 *   <token> <namespace> ro|rw     -> namespaced with explicit scope
 *   <token> admin                 -> global admin token
 * Returns 0 on success, -1 on a malformed scope. */
static int parse_token_line(Config *cfg, char *s) {
    char *tok = s;
    char *ns = next_field(s);
    if (!ns) return append_token(cfg, tok, NULL, AEGIS_SCOPE_ADMIN);

    char *scope_s = next_field(ns);
    if (strcmp(ns, "admin") == 0 && !scope_s)
        return append_token(cfg, tok, NULL, AEGIS_SCOPE_ADMIN);

    int scope = AEGIS_SCOPE_RW;
    if (scope_s) {
        if (strcmp(scope_s, "ro") == 0) scope = AEGIS_SCOPE_RO;
        else if (strcmp(scope_s, "rw") == 0) scope = AEGIS_SCOPE_RW;
        else if (strcmp(scope_s, "admin") == 0)
            return append_token(cfg, tok, NULL, AEGIS_SCOPE_ADMIN);
        else return -1; /* unknown scope */
    }
    return append_token(cfg, tok, ns, scope);
}

/* Load tokens from `path`, one per line, skipping blank lines and #-comments
 * and trimming surrounding whitespace. Returns 0 on success. */
static int load_token_file(Config *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    int rv = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;            /* skip leading ws */
        char *end = s + strlen(s);
        while (end > s && (end[-1] == '\n' || end[-1] == '\r' ||
                           end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';                              /* trim trailing ws */
        if (*s == '\0' || *s == '#') continue;          /* blank / comment */
        if (parse_token_line(cfg, s) != 0) { rv = -1; break; }
    }
    fclose(f);
    return rv;
}

static int parse_size(const char *s, size_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE) return -1;
    *out = (size_t)v;
    return 0;
}

static int parse_int(const char *s, int *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE || v < INT_MIN || v > INT_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]            run the server\n"
            "       %s client <op> [args]   talk to a server (try 'client')\n"
            "       %s gen-token [opts]      mint a token-file line + token\n"
            "       %s gen-key               mint an encryption-at-rest key\n"
            "\n"
            "Server options:\n"
            "  --data-dir <path>        persistence directory (default ./data)\n"
            "  --port <n>               TCP listen port (default 9470)\n"
            "  --phase <1-4>            highest enabled feature phase (default 4)\n"
            "  --io-threads <n>         poll() event-loop threads for dispatch\n"
            "                           parallelism (default: 2x CPUs, 8-64). Does\n"
            "                           not cap concurrent connections. Alias: --workers\n"
            "  --idle-timeout-sec <n>   close a connection idle (no byte progress)\n"
            "                           this long (default 60; 0 disables)\n"
            "  --max-connections <n>    hard cap on concurrent client connections\n"
            "                           (default 0 = unlimited)\n"
            "  --query-scan-cap <n>     max most-recent records a broad/filterless\n"
            "                           search or count loads (default 100000; 0=off)\n"
            "  --max-index-bytes <n>    soft cap on in-RAM index bytes; inserts get\n"
            "                           MEMORY_LIMIT past it (accepts K/M/G; 0=off)\n"
            "  --encryption-key-file <path>  encrypt the log at rest with the 32-byte\n"
            "                           key (64 hex chars) in <path>. On a NEW data dir\n"
            "                           it encrypts from the first write; on an existing\n"
            "                           plaintext dir run --encrypt-migrate first. Not\n"
            "                           yet combinable with replication\n"
            "  --encrypt-migrate        rewrite --data-dir's plaintext log encrypted\n"
            "                           (needs --encryption-key-file) and exit\n"

            "  --max-payload <bytes>    max data size (default 1048576)\n"
            "  --embedding-dim <n>      expected vector length (default 384)\n"
            "  --ann-ef-search <n>      HNSW query beam for large semantic indexes;\n"
            "                           higher = better recall, slower (default 50)\n"
            "  --ann-threshold <n>      live vectors before semantic search uses\n"
            "                           HNSW instead of an exact scan (default 10000)\n"
            "  --ann-quantize           store HNSW vectors as int8 (~4x less memory,\n"
            "                           small recall cost); default float32\n"
            "  --ann-shard-target <n>   target vectors per HNSW shard; the graph\n"
            "                           splits into ~count/n shards (capped by CPUs)\n"
            "                           so the build parallelizes (default 25000)\n"
            "  --tenant-max-records <n> per-namespace live-record cap; 0 = unlimited\n"
            "                           (enforced only when auth is enabled)\n"
            "  --tenant-max-bytes <n>   per-namespace live-byte cap; 0 = unlimited\n"
            "  --tenant-rate-qps <n>    per-namespace request rate limit (req/s,\n"
            "                           burst = 1s); 0 = unlimited\n"
            "  --replication-port <n>   serve the read-replica log stream on this\n"
            "                           port (requires --replication-token)\n"
            "  --replication-token <t>  token required to subscribe / sent when\n"
            "                           following a primary\n"
            "  --replicate-from <h:p>   follow this primary's replication port as a\n"
            "                           read-only replica (implies --read-only)\n"
            "  --read-only              refuse client writes (READ_ONLY)\n"
            "  --durability <mode>      sync|batch|interval (default interval)\n"
            "  --fsync-batch <n>        records between fsync in batch mode\n"
            "                           (default 1000)\n"
            "  --fsync-interval-ms <n>  flush cadence in interval mode\n"
            "                           (default 1000)\n"
            "  --checkpoint-sec <n>     index checkpoint cadence, 0 disables\n"
            "                           (default 60)\n"
            "  --compact-sec <n>        log-compaction check cadence; compacts only\n"
            "                           when >=25%% dead, 0 disables (default 300)\n"
            "  --working-capacity <n>   ring buffer size (default 256)\n"
            "  --auth-token <token>     accept this global admin token (repeatable)\n"
            "  --auth-token-file <path> accept tokens, one per line; each line is\n"
            "                           '<token> [namespace] [ro|rw|admin]' — a\n"
            "                           namespace binds the token to one tenant.\n"
            "                           A token may be 'sha256$<hex>' to store it\n"
            "                           hashed at rest (see --hash-token)\n"
            "  --hash-token <token>     print the token's 'sha256$<hex>' form and\n"
            "                           exit (paste into the token file)\n"
            "  --log-level <level>      error|warn|info|debug (default info,\n"
            "                           or $AEGISDB_LOG_LEVEL)\n"
            "  --health-check           probe a local server (--port) and exit\n"
            "  --restore <dir>          install the snapshot at <dir> into\n"
            "                           --data-dir and exit (data-dir must be\n"
            "                           empty); the next start recovers from it\n"
            "  --help                   show this help\n"
            "\n"
            "  With no --auth-token/--auth-token-file the server runs WITHOUT\n"
            "  authentication. Tokens are sent in plaintext; run the server\n"
            "  behind an encrypted channel (VPN, SSH tunnel, or TLS proxy).\n",
            prog, prog, prog, prog);
}

int config_parse_args(Config *cfg, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *prog = argv[0];
#define NEXT(name)                                                     \
    if (i + 1 >= argc) {                                               \
        fprintf(stderr, "%s: missing value for %s\n", prog, name);     \
        return -1;                                                     \
    }                                                                  \
    const char *val = argv[++i]

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(prog);
            return 1;
        } else if (strcmp(a, "--health-check") == 0) {
            cfg->run_health_check = 1;
        } else if (strcmp(a, "--hash-token") == 0) {
            NEXT("--hash-token");
            cfg->hash_token = val; /* borrows argv; printed and exits in main */
        } else if (strcmp(a, "--restore") == 0) {
            NEXT("--restore");
            cfg->restore_from = val; /* borrows argv; handled and exits in main */
        } else if (strcmp(a, "--encrypt-migrate") == 0) {
            cfg->encrypt_migrate = 1; /* handled and exits in main */
        } else if (strcmp(a, "--data-dir") == 0) {
            NEXT("--data-dir");
            strncpy(cfg->data_dir, val, sizeof(cfg->data_dir) - 1);
            cfg->data_dir[sizeof(cfg->data_dir) - 1] = '\0';
        } else if (strcmp(a, "--port") == 0) {
            NEXT("--port");
            if (parse_int(val, &cfg->listen_port) || cfg->listen_port <= 0 ||
                cfg->listen_port > 65535) {
                fprintf(stderr, "%s: invalid port '%s'\n", prog, val);
                return -1;
            }
        } else if (strcmp(a, "--phase") == 0) {
            NEXT("--phase");
            if (parse_int(val, &cfg->enabled_phase) || cfg->enabled_phase < 1 ||
                cfg->enabled_phase > 4) {
                fprintf(stderr, "%s: invalid phase '%s' (1-4)\n", prog, val);
                return -1;
            }
        } else if (strcmp(a, "--io-threads") == 0 || strcmp(a, "--workers") == 0) {
            NEXT(a); /* --workers kept as a back-compat alias for --io-threads */
            if (parse_int(val, &cfg->io_threads) || cfg->io_threads < 1) {
                fprintf(stderr, "%s: invalid %s '%s'\n", prog, a, val);
                return -1;
            }
        } else if (strcmp(a, "--idle-timeout-sec") == 0) {
            NEXT("--idle-timeout-sec");
            int v;
            if (parse_int(val, &v) || v < 0) {
                fprintf(stderr, "%s: invalid idle-timeout-sec '%s'\n", prog, val);
                return -1;
            }
            cfg->idle_timeout_sec = (unsigned)v;
        } else if (strcmp(a, "--max-connections") == 0) {
            NEXT("--max-connections");
            if (parse_int(val, &cfg->max_connections) || cfg->max_connections < 0) {
                fprintf(stderr, "%s: invalid max-connections '%s'\n", prog, val);
                return -1;
            }
        } else if (strcmp(a, "--query-scan-cap") == 0) {
            NEXT("--query-scan-cap");
            int v;
            if (parse_int(val, &v) || v < 0) {
                fprintf(stderr, "%s: invalid query-scan-cap '%s'\n", prog, val);
                return -1;
            }
            cfg->query_scan_cap = (size_t)v;
        } else if (strcmp(a, "--max-index-bytes") == 0) {
            NEXT("--max-index-bytes");
            if (parse_size(val, &cfg->max_index_bytes)) {
                fprintf(stderr, "%s: invalid max-index-bytes '%s'\n", prog, val);
                return -1;
            }
        } else if (strcmp(a, "--encryption-key-file") == 0) {
            NEXT("--encryption-key-file");
            if (load_key_file(val, cfg->encryption_key) != 0) {
                fprintf(stderr,
                        "%s: cannot read a 32-byte key (64 hex chars) from '%s'\n",
                        prog, val);
                return -1;
            }
            cfg->encryption_enabled = 1;
        } else if (strcmp(a, "--max-payload") == 0) {
            NEXT("--max-payload");
            if (parse_size(val, &cfg->max_payload_bytes)) {
                fprintf(stderr, "%s: invalid max-payload '%s'\n", prog, val);
                return -1;
            }
        } else if (strcmp(a, "--embedding-dim") == 0) {
            NEXT("--embedding-dim");
            if (parse_size(val, &cfg->embedding_dimensions)) {
                fprintf(stderr, "%s: invalid embedding-dim '%s'\n", prog, val);
                return -1;
            }
        } else if (strcmp(a, "--ann-ef-search") == 0) {
            NEXT("--ann-ef-search");
            if (parse_size(val, &cfg->ann_ef_search)) {
                fprintf(stderr, "%s: invalid ann-ef-search '%s'\n", prog, val);
                return -1;
            }
        } else if (strcmp(a, "--ann-threshold") == 0) {
            NEXT("--ann-threshold");
            if (parse_size(val, &cfg->ann_threshold)) {
                fprintf(stderr, "%s: invalid ann-threshold '%s'\n", prog, val);
                return -1;
            }
        } else if (strcmp(a, "--ann-shard-target") == 0) {
            NEXT("--ann-shard-target");
            if (parse_size(val, &cfg->ann_shard_target)) {
                fprintf(stderr, "%s: invalid ann-shard-target '%s'\n", prog, val);
                return -1;
            }
        } else if (strcmp(a, "--replication-port") == 0) {
            NEXT("--replication-port");
            if (parse_int(val, &cfg->replication_port) ||
                cfg->replication_port < 0 || cfg->replication_port > 65535) {
                fprintf(stderr, "%s: invalid replication-port '%s'\n", prog, val);
                return -1;
            }
        } else if (strcmp(a, "--replication-token") == 0) {
            NEXT("--replication-token");
            snprintf(cfg->replication_token, sizeof(cfg->replication_token),
                     "%s", val);
        } else if (strcmp(a, "--replicate-from") == 0) {
            NEXT("--replicate-from");
            /* host:port */
            const char *colon = strrchr(val, ':');
            if (!colon || colon == val) {
                fprintf(stderr, "%s: --replicate-from wants host:port, got '%s'\n",
                        prog, val);
                return -1;
            }
            size_t hlen = (size_t)(colon - val);
            if (hlen >= sizeof(cfg->replicate_from_host)) hlen = sizeof(cfg->replicate_from_host) - 1;
            memcpy(cfg->replicate_from_host, val, hlen);
            cfg->replicate_from_host[hlen] = '\0';
            if (parse_int(colon + 1, &cfg->replicate_from_port) ||
                cfg->replicate_from_port <= 0 || cfg->replicate_from_port > 65535) {
                fprintf(stderr, "%s: invalid port in --replicate-from '%s'\n", prog, val);
                return -1;
            }
            cfg->read_only = 1; /* a replica never accepts client writes */
        } else if (strcmp(a, "--read-only") == 0) {
            cfg->read_only = 1;
        } else if (strcmp(a, "--ann-quantize") == 0) {
            cfg->ann_quantize = 1; /* boolean flag: int8 HNSW vectors */
        } else if (strcmp(a, "--tenant-max-records") == 0) {
            NEXT("--tenant-max-records");
            if (parse_size(val, &cfg->tenant_max_records)) {
                fprintf(stderr, "%s: invalid tenant-max-records '%s'\n", prog, val);
                return -1;
            }
        } else if (strcmp(a, "--tenant-max-bytes") == 0) {
            NEXT("--tenant-max-bytes");
            if (parse_size(val, &cfg->tenant_max_bytes)) {
                fprintf(stderr, "%s: invalid tenant-max-bytes '%s'\n", prog, val);
                return -1;
            }
        } else if (strcmp(a, "--tenant-rate-qps") == 0) {
            NEXT("--tenant-rate-qps");
            char *end = NULL;
            double q = strtod(val, &end);
            if (end == val || *end != '\0' || q < 0) {
                fprintf(stderr, "%s: invalid tenant-rate-qps '%s'\n", prog, val);
                return -1;
            }
            cfg->tenant_rate_qps = q;
        } else if (strcmp(a, "--fsync-batch") == 0) {
            NEXT("--fsync-batch");
            if (parse_size(val, &cfg->fsync_batch_size)) {
                fprintf(stderr, "%s: invalid fsync-batch '%s'\n", prog, val);
                return -1;
            }
        } else if (strcmp(a, "--durability") == 0) {
            NEXT("--durability");
            if (aegis_durability_from_string(val, &cfg->durability) != 0) {
                fprintf(stderr, "%s: invalid durability '%s' "
                                "(sync|batch|interval)\n",
                        prog, val);
                return -1;
            }
        } else if (strcmp(a, "--fsync-interval-ms") == 0) {
            NEXT("--fsync-interval-ms");
            size_t ms;
            if (parse_size(val, &ms) || ms == 0) {
                fprintf(stderr, "%s: invalid fsync-interval-ms '%s'\n", prog,
                        val);
                return -1;
            }
            cfg->fsync_interval_ms = (uint64_t)ms;
        } else if (strcmp(a, "--checkpoint-sec") == 0) {
            NEXT("--checkpoint-sec");
            int cs;
            if (parse_int(val, &cs) || cs < 0) {
                fprintf(stderr, "%s: invalid checkpoint-sec '%s'\n", prog, val);
                return -1;
            }
            cfg->checkpoint_sec = (unsigned)cs;
        } else if (strcmp(a, "--compact-sec") == 0) {
            NEXT("--compact-sec");
            int cs;
            if (parse_int(val, &cs) || cs < 0) {
                fprintf(stderr, "%s: invalid compact-sec '%s'\n", prog, val);
                return -1;
            }
            cfg->compact_sec = (unsigned)cs;
        } else if (strcmp(a, "--working-capacity") == 0) {
            NEXT("--working-capacity");
            int wc;
            if (parse_int(val, &wc) || wc < 1) {
                fprintf(stderr, "%s: invalid working-capacity '%s'\n", prog, val);
                return -1;
            }
            cfg->working_capacity = (uint32_t)wc;
        } else if (strcmp(a, "--auth-token") == 0) {
            NEXT("--auth-token");
            if (append_token(cfg, val, NULL, AEGIS_SCOPE_ADMIN) != 0) {
                fprintf(stderr, "%s: out of memory adding auth token\n", prog);
                return -1;
            }
        } else if (strcmp(a, "--auth-token-file") == 0) {
            NEXT("--auth-token-file");
            if (load_token_file(cfg, val) != 0) {
                fprintf(stderr, "%s: cannot read auth-token-file '%s'\n", prog,
                        val);
                return -1;
            }
            snprintf(cfg->auth_token_file, sizeof(cfg->auth_token_file), "%s",
                     val); /* retained so token-admin ops can persist */
        } else if (strcmp(a, "--log-level") == 0) {
            NEXT("--log-level");
            AegisLogLevel lvl;
            if (aegis_log_level_from_string(val, &lvl) != 0) {
                fprintf(stderr, "%s: invalid log-level '%s' "
                                "(error|warn|info|debug)\n",
                        prog, val);
                return -1;
            }
            cfg->log_level = lvl;
        } else {
            fprintf(stderr, "%s: unknown argument '%s'\n", prog, a);
            usage(prog);
            return -1;
        }
#undef NEXT
    }
    return 0;
}

void config_free(Config *cfg) {
    if (!cfg || !cfg->auth_tokens) return;
    for (size_t i = 0; i < cfg->auth_token_count; i++) {
        free(cfg->auth_tokens[i].token);
        free(cfg->auth_tokens[i].namespace);
    }
    free(cfg->auth_tokens);
    cfg->auth_tokens = NULL;
    cfg->auth_token_count = 0;
}

/* ----- runtime token administration ------------------------------------- */

int config_add_token(Config *cfg, const char *tok, const char *ns, int scope) {
    return append_token(cfg, tok, ns, scope);
}

/* SHA-256 of a token: its stored digest if hashed, else computed from plaintext.
 * The hash is unsalted — sufficient only because tokens are expected to be
 * high-entropy random secrets (see sha256.h; `gen-token` produces such tokens,
 * and the docs direct operators to `openssl rand -hex 32`). A low-entropy token
 * would be vulnerable to precomputation; salting is not a substitute for entropy. */
static void token_digest(const AuthToken *t, uint8_t out[32]) {
    if (t->hashed)
        memcpy(out, t->hash, 32);
    else
        sha256((const uint8_t *)t->token, strlen(t->token), out);
}

static void hex_encode(const uint8_t *in, size_t n, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hx[in[i] >> 4];
        out[2 * i + 1] = hx[in[i] & 0xf];
    }
    out[2 * n] = '\0';
}

void config_token_fingerprint(const AuthToken *t, char out[13]) {
    uint8_t d[32];
    token_digest(t, d);
    hex_encode(d, 6, out); /* first 6 bytes -> 12 hex chars */
}

void config_key_fingerprint(const uint8_t key[AEAD_KEY_LEN], char out[13]) {
    uint8_t d[SHA256_DIGEST_LEN];
    sha256(key, AEAD_KEY_LEN, d);
    hex_encode(d, 6, out); /* first 6 bytes of SHA-256(key) -> 12 hex chars */
}

int config_remove_token(Config *cfg, const char *id12) {
    for (size_t i = 0; i < cfg->auth_token_count; i++) {
        char fp[13];
        config_token_fingerprint(&cfg->auth_tokens[i], fp);
        if (strcmp(fp, id12) == 0) {
            free(cfg->auth_tokens[i].token);
            free(cfg->auth_tokens[i].namespace);
            memmove(&cfg->auth_tokens[i], &cfg->auth_tokens[i + 1],
                    (cfg->auth_token_count - i - 1) * sizeof(AuthToken));
            cfg->auth_token_count--;
            return 1;
        }
    }
    return 0;
}

static const char *scope_name(int scope) {
    return scope == AEGIS_SCOPE_RO ? "ro" : scope == AEGIS_SCOPE_ADMIN ? "admin"
                                                                       : "rw";
}

int config_write_token_file(const Config *cfg, const char *path) {
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    /* Create the temp file 0600 from the start (not fopen's umask-dependent mode
     * then a later chmod, which leaves a world-readable window). Clear any stale
     * .tmp first so O_EXCL then refuses to follow a pre-planted symlink. */
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    int ok = 1;
    for (size_t i = 0; i < cfg->auth_token_count && ok; i++) {
        const AuthToken *t = &cfg->auth_tokens[i];
        uint8_t d[32];
        char hex[65];
        token_digest(t, d);
        hex_encode(d, 32, hex);
        int n = t->namespace
                    ? fprintf(f, "sha256$%s %s %s\n", hex, t->namespace,
                              scope_name(t->scope))
                    : fprintf(f, "sha256$%s\n", hex); /* global admin */
        if (n < 0) ok = 0;
    }
    if (fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0; /* closes fd; file was created 0600 */
    if (ok && rename(tmp, path) != 0) ok = 0;
    if (!ok) unlink(tmp);
    return ok ? 0 : -1;
}