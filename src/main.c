/* AegisDB entry point: config -> recovery -> server (T019, T025). */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aegisdb/compaction.h"
#include "aegisdb/config.h"
#include "aegisdb/db.h"
#include "aegisdb/health.h"
#include "aegisdb/logging.h"
#include "aegisdb/tcp_server.h"

static void on_signal(int sig) {
    (void)sig;
    tcp_server_request_stop();
}

int main(int argc, char **argv) {
    Config cfg;
    config_defaults(&cfg);
    int pr = config_parse_args(&cfg, argc, argv);
    if (pr != 0) { config_free(&cfg); return pr < 0 ? 1 : 0; } /* -1 err, 1 help */
    aegis_log_set_level((AegisLogLevel)cfg.log_level);

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

    /* Maintenance: sweep expired working memory every 30s; compaction is
     * opt-in via signal/age in future — disabled on the timer by default. */
    Compactor *maint = compaction_start(&db, 30, 0);
    if (maint)
        LOG_DEBUG("maintenance thread started (working-memory sweep every 30s)");
    else
        LOG_WARN("could not start maintenance thread; "
                 "expired working memory will not be swept");

    int rv = tcp_server_run(&db);

    LOG_INFO("stopping");
    compaction_stop(maint);
    db_close(&db);
    config_free(&cfg); /* db.config shares this token array (shallow copy) */
    return rv == 0 ? 0 : 1;
}