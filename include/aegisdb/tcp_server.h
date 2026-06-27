/* TCP NDJSON server (T018). */
#ifndef AEGISDB_TCP_SERVER_H
#define AEGISDB_TCP_SERVER_H

#include "aegisdb/db.h"

/* Bind, listen, and serve requests until stop is requested. Blocks. 0/-1. */
int tcp_server_run(AegisDB *db);

/* Async-signal-safe: ask the accept loop to stop. */
void tcp_server_request_stop(void);

#endif /* AEGISDB_TCP_SERVER_H */