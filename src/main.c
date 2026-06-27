/* AegisDB entry point: config -> recovery -> server (T019, T025). */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aegisdb/compaction.h"
#include "aegisdb/config.h"
#include "aegisdb/db.h"
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

    if (cfg.auth_token_count == 0)
        fprintf(stderr, "[aegisdb] WARNING: no auth tokens configured; the "
                        "server accepts unauthenticated requests from anyone "
                        "who can reach the port\n");

    AegisDB db;
    if (db_open(&db, &cfg) != 0) {
        fprintf(stderr, "[aegisdb] failed to open database\n");
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

    int rv = tcp_server_run(&db);

    compaction_stop(maint);
    db_close(&db);
    config_free(&cfg); /* db.config shares this token array (shallow copy) */
    return rv == 0 ? 0 : 1;
}