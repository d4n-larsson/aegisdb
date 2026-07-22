/* Offline plaintext -> encrypted log migration (see encrypt_migrate.h). */
#include "aegisdb/encrypt_migrate.h"

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "aegisdb/fsutil.h"
#include "aegisdb/log.h"
#include "aegisdb/logging.h"

typedef struct {
    LogFile *dst;
    int err;
} MigrateCtx;

/* Re-append each plaintext frame from the source scan into the encrypted dest,
 * which seals it with a fresh nonce. Aborting the scan on the first write error. */
static int migrate_cb(uint64_t off, const uint8_t *payload, size_t len,
                      void *ctx) {
    (void)off;
    MigrateCtx *m = ctx;
    if (log_append(m->dst, payload, len, NULL) != 0) {
        m->err = 1;
        return 1;
    }
    return 0;
}

int encrypt_migrate_run(const Config *cfg) {
    if (!cfg->encryption_enabled) {
        fprintf(stderr,
                "--encrypt-migrate requires --encryption-key-file\n");
        return -1;
    }

    char path_log[1100], path_new[1200], path_index[1100], path_sem[1100];
    snprintf(path_log, sizeof path_log, "%s/memory.log", cfg->data_dir);
    snprintf(path_new, sizeof path_new, "%s/memory.log.enc.tmp", cfg->data_dir);
    snprintf(path_index, sizeof path_index, "%s/memory.index", cfg->data_dir);
    snprintf(path_sem, sizeof path_sem, "%s/memory.sem", cfg->data_dir);

    /* Open the source plaintext (NULL key). This also migrates a legacy v1 log
     * to v2 in place. An already-encrypted log is refused by log_open. */
    LogFile src;
    LogOpenStatus st;
    if (log_open(&src, path_log, 0, NULL, &st) != 0) {
        if (st == LOG_OPEN_ERR_PLAIN_ON_ENC)
            LOG_ERROR("encrypt-migrate: %s is already encrypted; nothing to do",
                      path_log);
        else
            LOG_ERROR("encrypt-migrate: cannot open %s", path_log);
        return -1;
    }

    /* Fresh encrypted scratch log; the original is untouched until the rename. */
    unlink(path_new);
    LogFile dst;
    if (log_open(&dst, path_new, 0, cfg->encryption_key, NULL) != 0) {
        LOG_ERROR("encrypt-migrate: cannot create scratch log %s", path_new);
        log_close(&src);
        return -1;
    }

    MigrateCtx mc = {&dst, 0};
    LogScanResult res;
    int scan_rv = log_scan(&src, 0, migrate_cb, &mc, &res);
    log_close(&src);
    if (scan_rv != 0 || mc.err) {
        LOG_ERROR("encrypt-migrate: failed while rewriting frames");
        log_close(&dst);
        unlink(path_new);
        return -1;
    }
    log_fsync(&dst);
    log_close(&dst);

    /* Atomically swap the encrypted log in for the plaintext one. */
    if (rename(path_new, path_log) != 0) {
        LOG_ERROR("encrypt-migrate: cannot install %s", path_log);
        unlink(path_new);
        return -1;
    }
    /* Make the swap durable: fsync the directory so the rename survives a crash. */
    fs_fsync_parent(path_log);

    /* Drop the now-stale plaintext checkpoints; the next start rebuilds encrypted
     * ones from the log. (Even if left, a plaintext checkpoint fails to decrypt
     * under the key and is treated as missing — deleting is just tidy.) */
    unlink(path_index);
    unlink(path_sem);

    if (res.corrupt_frames)
        LOG_WARN("encrypt-migrate: skipped %zu corrupt frame(s)",
                 res.corrupt_frames);
    LOG_INFO("encrypt-migrate: %s is now encrypted (%zu frame(s)); start the "
             "server with the same --encryption-key-file",
             path_log, res.good_frames);
    return 0;
}