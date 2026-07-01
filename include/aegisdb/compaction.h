/* Background log compaction + working-memory TTL sweep (T033). */
#ifndef AEGISDB_COMPACTION_H
#define AEGISDB_COMPACTION_H

#include "aegisdb/db.h"

typedef struct Compactor Compactor;

/* Run one synchronous compaction pass: rewrite live records to a fresh log,
 * atomically swap it in, and rebuild the hash index with new offsets. Drops
 * tombstones and superseded versions. Returns 0/-1. */
int compaction_run_once(AegisDB *db);

/* True when compaction is worth the full-log rewrite: at least 25% of the hash
 * entries are tombstones (dead). The maintenance thread gates its scheduled
 * compaction on this. Exposed for testing. */
int compaction_worthwhile(AegisDB *db);

/* Start a maintenance thread that sweeps expired working memory every
 * `sweep_sec` and compacts every `compact_sec` (0 disables compaction). */
Compactor *compaction_start(AegisDB *db, unsigned sweep_sec, unsigned compact_sec);
void compaction_stop(Compactor *c);

#endif /* AEGISDB_COMPACTION_H */