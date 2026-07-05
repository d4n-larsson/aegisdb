/* Snapshot restore: the one-shot `--restore <dir>` mode.
 *
 * A snapshot (see db_snapshot) is a self-contained data set — memory.log,
 * metadata.db, manifest.json. Restore validates the manifest against the target
 * configuration and installs the log + metadata into an empty --data-dir, from
 * which the next normal start rebuilds every index. */
#ifndef AEGISDB_RESTORE_H
#define AEGISDB_RESTORE_H

#include "aegisdb/config.h"

/* Install the snapshot at cfg->restore_from into cfg->data_dir, then the caller
 * exits. Validates manifest.json (format + embedding_dim must match cfg) and
 * refuses to overwrite an existing database (a memory.log already in data_dir).
 * Returns 0 on success, -1 on error (a diagnostic is logged). */
int restore_run(const Config *cfg);

#endif /* AEGISDB_RESTORE_H */