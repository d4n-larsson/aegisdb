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
#include "aegisdb/replication.h"
#include "aegisdb/restore.h"
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
    if (argc >= 2 && strcmp(argv[1], "gen-key") == 0)
        return gen_key_main(argc - 1, argv + 1);

    Config cfg;
    config_defaults(&cfg);
    int pr = config_parse_args(&cfg, argc, argv);
    if (pr != 0) { config_free(&cfg); return pr < 0 ? 1 : 0; } /* -1 err, 1 help */
    aegis_log_set_level((AegisLogLevel)cfg.log_level);

    /* Encryption at rest is being rolled out incrementally (see
     * docs/encryption-at-rest-design.md). The log is encrypted; wiring the
     * checkpoints, backup/restore, and replication paths lands in later PRs, so
     * refuse the combinations that are not yet safe rather than run them wrong. */
    if (cfg.encryption_enabled) {
        if (cfg.replication_port > 0 || cfg.replicate_from_host[0] != '\0') {
            fprintf(stderr, "encryption at rest is not yet supported together "
                            "with replication\n");
            config_free(&cfg);
            return 1;
        }
        if (cfg.restore_from) {
            fprintf(stderr, "encryption at rest is not yet supported together "
                            "with --restore\n");
            config_free(&cfg);
            return 1;
        }
    }

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

    /* One-shot: install a snapshot into --data-dir, then exit; a normal start
     * afterwards recovers from it. */
    if (cfg.restore_from) {
        int rr = restore_run(&cfg);
        config_free(&cfg);
        return rr == 0 ? 0 : 1;
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

    if (cfg.encryption_enabled) {
        uint8_t d[SHA256_DIGEST_LEN];
        sha256(cfg.encryption_key, AEAD_KEY_LEN, d);
        LOG_INFO("encryption at rest: ENABLED (log sealed with XChaCha20-Poly1305; "
                 "key fingerprint %02x%02x%02x%02x%02x%02x)", d[0], d[1], d[2],
                 d[3], d[4], d[5]);
        LOG_WARN("encrypted mode: index checkpoints are disabled for now, so "
                 "recovery full-scans the log (slower startup on a large log)");
    } else if (cfg.checkpoint_sec)
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

    /* Replication (Phase 1 read replicas). Primary: serve the log stream if a
     * port + token are configured. Replica: follow the primary. Both are
     * independent of the client server below. */
    if (cfg.replication_port > 0) {
        if (cfg.replication_token[0] == '\0') {
            LOG_ERROR("--replication-port requires --replication-token");
            compaction_stop(maint);
            db_close(&db);
            config_free(&cfg);
            return 1;
        }
        db.repl_source = replication_source_start(&db, cfg.replication_port,
                                                  cfg.replication_token);
        if (!db.repl_source)
            LOG_WARN("replication: source failed to start; no replicas can follow");
    }
    if (cfg.replicate_from_host[0] != '\0') {
        db.repl_follower = replication_follower_start(
            &db, cfg.replicate_from_host, cfg.replicate_from_port,
            cfg.replication_token);
        if (!db.repl_follower)
            LOG_WARN("replication: follower failed to start; replica will not sync");
        LOG_INFO("read-only replica of %s:%d", cfg.replicate_from_host,
                 cfg.replicate_from_port);
    }

    int rv = tcp_server_run(&db);

    LOG_INFO("stopping");
    replication_follower_stop(db.repl_follower);
    replication_source_stop(db.repl_source);
    compaction_stop(maint);
    db_close(&db);
    config_free(&cfg); /* db.config shares this token array (shallow copy) */
    return rv == 0 ? 0 : 1;
}