/* Startup recovery: scan log, validate checksums, rebuild indexes (T013).
 * Extended to rebuild time/tag (T034) and semantic (T040) indexes. */
#ifndef AEGISDB_RECOVERY_H
#define AEGISDB_RECOVERY_H

#include "aegisdb/db.h"

/* Rebuild all in-memory indexes from the append-only log, dropping any torn
 * tail, and set db->next_id to max(persisted id)+1. Returns the number of live
 * records loaded, or -1 on error. */
long recovery_run(AegisDB *db);

#endif /* AEGISDB_RECOVERY_H */