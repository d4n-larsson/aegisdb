/* Offline plaintext -> encrypted migration for the log (encryption at rest, see
 * docs/encryption-at-rest-design.md). Invoked as a one-shot:
 *
 *   aegisdb --encrypt-migrate --data-dir <dir> --encryption-key-file <key>
 *
 * Rewrites the plaintext memory.log into an encrypted (v3) log under the key,
 * atomically (scratch file + rename; the original is untouched until the swap
 * succeeds), and drops the plaintext checkpoints so the next start rebuilds
 * encrypted ones. The server does NOT auto-migrate on open (a plaintext log +
 * key is refused) — this is the explicit, opt-in path. */
#ifndef AEGISDB_ENCRYPT_MIGRATE_H
#define AEGISDB_ENCRYPT_MIGRATE_H

#include "aegisdb/config.h"

/* Returns 0 on success, -1 on failure (nothing is changed on failure). */
int encrypt_migrate_run(const Config *cfg);

#endif /* AEGISDB_ENCRYPT_MIGRATE_H */