/* One-shot health probe: connect to a running server and verify it responds.
 *
 * Used by `aegisdb --health-check` (and the container HEALTHCHECK) to check
 * liveness without any external tooling. */
#ifndef AEGISDB_HEALTH_H
#define AEGISDB_HEALTH_H

/* Connect to 127.0.0.1:<port>, send a `ping`, and confirm an ok response.
 * Returns 0 if the server is healthy, non-zero otherwise (connection refused,
 * timeout, or an unexpected reply). Never blocks longer than a few seconds. */
int health_check(int port);

#endif /* AEGISDB_HEALTH_H */