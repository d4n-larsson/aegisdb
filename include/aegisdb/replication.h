/* Phase 1 read replicas: log shipping over a dedicated TCP port.
 *
 * A primary opens a replication *source* that streams its append-only log
 * frames to subscribed replicas; a replica runs a *follower* that connects,
 * streams frames from its cursor, appends them verbatim to its own log
 * (byte-identical → same offsets), and applies each to its in-memory indexes.
 * Read-only; manual promotion. See docs/read-replica-design.md.
 *
 * The wire framing on the dedicated port: a one-line NDJSON handshake
 *   replica → primary: {"from_offset":N,"generation":G,"token":"…"}
 *   primary → replica: {"ok":true,"generation":G,"reset":bool}
 * then a stream of binary messages primary → replica:
 *   [MAGIC u32][type u8][offset u64][len u32][payload len bytes]
 * type: 0 FRAME (payload = a log frame's payload), 1 HEARTBEAT (len 0, offset =
 * primary log size), 2 RESET (len 0; the primary compacted → wipe & resubscribe).
 * All integers little-endian, matching the on-disk log format.
 */
#ifndef AEGISDB_REPLICATION_H
#define AEGISDB_REPLICATION_H

#include <stdint.h>

#include "aegisdb/db.h" /* AegisDB; db.h only uses our struct tags, so no cycle */

typedef struct ReplicationSource ReplicationSource;
typedef struct ReplicationFollower ReplicationFollower;

/* --- primary: serve the log stream on `port`, requiring `token` ------------ */
ReplicationSource *replication_source_start(AegisDB *db, int port,
                                            const char *token);
void replication_source_stop(ReplicationSource *s);
/* Number of currently-connected replicas (for stats). */
int replication_source_replica_count(ReplicationSource *s);

/* --- replica: follow the primary at host:port ------------------------------ */
ReplicationFollower *replication_follower_start(AegisDB *db, const char *host,
                                                int port, const char *token);
void replication_follower_stop(ReplicationFollower *f);
/* Snapshot follower status for stats: applied byte offset (local log size), the
 * primary's last-reported log size, and whether the stream is currently up. */
void replication_follower_status(ReplicationFollower *f, uint64_t *applied,
                                 uint64_t *primary_size, int *connected);

#endif /* AEGISDB_REPLICATION_H */