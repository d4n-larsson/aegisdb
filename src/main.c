/* AegisDB entry point: config -> recovery -> server (T019, T025). */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aegisdb/client.h"
#include "aegisdb/compaction.h"
#include "aegisdb/config.h"
#include "aegisdb/db.h"
#include "aegisdb/health.h"
#include "aegisdb/logging.h"
#include "aegisdb/sha256.h"
#include "aegisdb/tcp_server.h"

static void on_signal(int sig) {
    (void)sig;
    int saved = errno; /* a handler must not clobber the interrupted code's errno */
    tcp_server_request_stop();
    errno = saved;
}

int main(int argc, char **argv) {
    /* Subcommands run a client / token tool instead of the server. */
    if (argc >= 2 && strcmp(argv[1], "client") == 0)
        return client_main(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "gen-token") == 0)
        return gen_token_main(argc - 1, argv + 1);

    Config cfg;
    config_defaults(&cfg);
    int pr = config_parse_args(&cfg, argc, argv);
    if (pr != 0) { config_free(&cfg); return pr < 0 ? 1 : 0; } /* -1 err, 1 help */
    aegis_log_set_level((AegisLogLevel)cfg.log_level);

    /* One-shot: print the hashed form of a token for the token file, then exit. */
    if (cfg.hash_token) {
        uint8_t d[SHA256_DIGEST_LEN];
        sha256(cfg.hash_token, strlen(cfg.hash_token), d);
        char hex[2 * SHA256_DIGEST_LEN + 1];
        for (int i = 0; i < SHA256_DIGEST_LEN; i++)
            snprintf(hex + i * 2, 3, "%02x", d[i]);
        printf("sha256$%s\n", hex);
        config_free(&cfg);
        return 0;
    }

    /* One-shot liveness probe (container HEALTHCHECK): connect to a running
     * server on --port, ping it, and exit with the result. */
    if (cfg.run_health_check) {
        int hc = health_check(cfg.listen_port);
        config_free(&cfg);
        return hc == 0 ? 0 : 1;
    }

    LOG_INFO("AegisDB %s starting (log level: %s)", AEGIS_VERSION_STRING,
             aegis_log_level_name((AegisLogLevel)cfg.log_level));

    if (cfg.auth_token_count == 0)
        LOG_WARN("no auth tokens configured; the server accepts "
                 "unauthenticated requests from anyone who can reach the port");
    else
        LOG_INFO("authentication enabled (%zu token%s configured)",
                 cfg.auth_token_count, cfg.auth_token_count == 1 ? "" : "s");

    LOG_INFO("durability: %s", aegis_durability_name(cfg.durability));
    if (cfg.durability == AEGIS_DURABILITY_BATCH && cfg.fsync_batch_size > 1)
        LOG_WARN("durability=batch: up to %zu acknowledged write(s) may be lost "
                 "on crash, and an idle server may leave them unflushed "
                 "indefinitely; use --durability sync or interval to bound this",
                 cfg.fsync_batch_size);
    else if (cfg.durability == AEGIS_DURABILITY_INTERVAL)
        LOG_INFO("durability=interval: log flushed every ~%llu ms; "
                 "acknowledged writes within that window may be lost on crash",
                 (unsigned long long)cfg.fsync_interval_ms);

    if (cfg.checkpoint_sec)
        LOG_INFO("index checkpoint every %us (recovery replays only the tail "
                 "written since the last checkpoint)",
                 cfg.checkpoint_sec);
    else
        LOG_WARN("index checkpoints disabled; recovery will full-scan the log");

    AegisDB db;
    if (db_open(&db, &cfg) != 0) {
        LOG_ERROR("failed to open database");
        config_free(&cfg);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Maintenance: sweep expired working memory every 30s, checkpoint the index
     * on its cadence, (in INTERVAL durability) flush the log on its cadence, and
     * compact the log every --compact-sec (only when >=25% of it is dead). */
    Compactor *maint = compaction_start(&db, 30, cfg.compact_sec);
    if (maint)
        LOG_DEBUG("maintenance thread started (working-memory sweep every 30s, "
                  "compaction check every %us)", cfg.compact_sec);
    else {
        LOG_WARN("could not start maintenance thread; "
                 "expired working memory will not be swept");
        if (cfg.durability == AEGIS_DURABILITY_INTERVAL)
            LOG_WARN("durability=interval needs the maintenance thread; the log "
                     "will only be flushed on shutdown");
    }

    int rv = tcp_server_run(&db);

    LOG_INFO("stopping");
    compaction_stop(maint);
    db_close(&db);
    config_free(&cfg); /* db.config shares this token array (shallow copy) */
    return rv == 0 ? 0 : 1;
}