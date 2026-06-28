/* Server configuration defaults and CLI parsing (T008). */
#include "aegisdb/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aegisdb/logging.h"

void config_defaults(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->listen_port = 9470;
    strncpy(cfg->data_dir, "./data", sizeof(cfg->data_dir) - 1);
    cfg->max_payload_bytes = 1048576; /* 1 MiB */
    cfg->embedding_dimensions = 384;
    cfg->working_capacity = 256;
    cfg->default_ttl_ms = 3600000; /* 1 hour */
    cfg->fsync_batch_size = 1000;
    cfg->durability = AEGIS_DURABILITY_INTERVAL;
    cfg->fsync_interval_ms = 1000;
    cfg->checkpoint_sec = 60;
    cfg->worker_threads = 4;
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

/* Append a copy of `tok` to the accepted-token list. Returns 0 on success. */
static int append_token(Config *cfg, const char *tok) {
    char **grown = realloc(cfg->auth_tokens,
                           (cfg->auth_token_count + 1) * sizeof(char *));
    if (!grown) return -1;
    cfg->auth_tokens = grown;
    char *copy = strdup(tok);
    if (!copy) return -1;
    cfg->auth_tokens[cfg->auth_token_count++] = copy;
    return 0;
}

/* Load one token per line from `path`, skipping blank lines and #-comments and
 * trimming surrounding whitespace. Returns 0 on success. */
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
        if (append_token(cfg, s) != 0) { rv = -1; break; }
    }
    fclose(f);
    return rv;
}

static int parse_size(const char *s, size_t *out) {
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0') return -1;
    *out = (size_t)v;
    return 0;
}

static int parse_int(const char *s, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return -1;
    *out = (int)v;
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --data-dir <path>        persistence directory (default ./data)\n"
            "  --port <n>               TCP listen port (default 9470)\n"
            "  --phase <1-4>            highest enabled feature phase (default 4)\n"
            "  --workers <n>            worker thread count (default 4)\n"
            "  --max-payload <bytes>    max data size (default 1048576)\n"
            "  --embedding-dim <n>      expected vector length (default 384)\n"
            "  --durability <mode>      sync|batch|interval (default interval)\n"
            "  --fsync-batch <n>        records between fsync in batch mode\n"
            "                           (default 1000)\n"
            "  --fsync-interval-ms <n>  flush cadence in interval mode\n"
            "                           (default 1000)\n"
            "  --checkpoint-sec <n>     index checkpoint cadence, 0 disables\n"
            "                           (default 60)\n"
            "  --working-capacity <n>   ring buffer size (default 256)\n"
            "  --auth-token <token>     accept this bearer token (repeatable)\n"
            "  --auth-token-file <path> accept tokens listed one per line\n"
            "  --log-level <level>      error|warn|info|debug (default info,\n"
            "                           or $AEGISDB_LOG_LEVEL)\n"
            "  --health-check           probe a local server (--port) and exit\n"
            "  --help                   show this help\n"
            "\n"
            "  With no --auth-token/--auth-token-file the server runs WITHOUT\n"
            "  authentication. Tokens are sent in plaintext; run the server\n"
            "  behind an encrypted channel (VPN, SSH tunnel, or TLS proxy).\n",
            prog);
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
        } else if (strcmp(a, "--workers") == 0) {
            NEXT("--workers");
            if (parse_int(val, &cfg->worker_threads) || cfg->worker_threads < 1) {
                fprintf(stderr, "%s: invalid workers '%s'\n", prog, val);
                return -1;
            }
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
            if (append_token(cfg, val) != 0) {
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
    for (size_t i = 0; i < cfg->auth_token_count; i++)
        free(cfg->auth_tokens[i]);
    free(cfg->auth_tokens);
    cfg->auth_tokens = NULL;
    cfg->auth_token_count = 0;
}