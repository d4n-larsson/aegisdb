/* Fuzz target: the wire request path (json_request.c + dispatch).
 *
 * aegis_request_handle takes one raw NDJSON line straight off a client socket
 * and runs the whole front end: cJSON parse, the jr_* field extractors
 * (string/number/bool/string-array/float-array), operation dispatch, and
 * response encoding. This is the surface that malformed or hostile client
 * input hits first, so we fuzz the bytes as the request line.
 *
 * The handler needs a live AegisDB. We open one lazily against a throwaway
 * temp data dir and reuse it across inputs for throughput — db_open() spawns no
 * background threads (the maintenance thread lives in main.c), so a reused DB
 * stays deterministic. State does accumulate (accepted inserts persist to the
 * temp log), which is fine for the time-bounded runs we do; a long soak should
 * point TMPDIR at scratch space.
 *
 * Built under libFuzzer (`make fuzz`) or with standalone_main.c for
 * deterministic corpus replay (`make fuzz-regress`).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aegisdb/config.h"
#include "aegisdb/db.h"
#include "aegisdb/json_request.h"
#include "aegisdb/logging.h"

static AegisDB g_db;
static int g_ready;

/* Close the reused DB at process exit so leak detection stays meaningful (the
 * open DB's allocations would otherwise look like leaks). */
static void fuzz_db_cleanup(void) {
    if (g_ready)
        db_close(&g_db);
}

static void fuzz_db_init(void) {
    char tmpl[] = "/tmp/aegis-fuzz-XXXXXX";
    const char *env = getenv("TMPDIR");
    char path[1024];
    if (env && *env)
        snprintf(path, sizeof(path), "%s/aegis-fuzz-XXXXXX", env);
    else
        snprintf(path, sizeof(path), "%s", tmpl);
    if (!mkdtemp(path))
        abort(); /* can't fuzz without a data dir */

    aegis_log_set_level(AEGIS_LOG_ERROR); /* silence per-open lifecycle noise */

    Config cfg;
    config_defaults(&cfg);
    snprintf(cfg.data_dir, sizeof(cfg.data_dir), "%s", path);
    cfg.embedding_dimensions = 8; /* small: keep encode/validate cheap */
    if (db_open(&g_db, &cfg) != 0)
        abort();
    g_ready = 1;
    atexit(fuzz_db_cleanup);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!g_ready)
        fuzz_db_init();

    size_t out_len = 0;
    char *resp = aegis_request_handle(&g_db, (const char *)data, size, &out_len);
    free(resp);
    return 0;
}