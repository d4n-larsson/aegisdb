/* Blocking TCP client-side socket helpers shared by the built-in client, the
 * replication follower, and the health probe. */
#ifndef AEGISDB_NETIO_H
#define AEGISDB_NETIO_H

#include <stddef.h>
#include <stdint.h>

/* Connect to host:port (TCP, AF_UNSPEC so IPv4/IPv6 both work). `port` is a
 * numeric string or service name. Returns a connected socket fd, or -1. */
int net_dial(const char *host, const char *port);

/* Bound blocking recv/send with SO_RCVTIMEO/SO_SNDTIMEO = `secs`, and disable
 * Nagle (TCP_NODELAY). Best-effort: setsockopt failures are ignored. */
void net_set_timeouts(int fd, int secs);

/* Write exactly `len` bytes, retrying short writes and EINTR. Returns 0 on
 * success, -1 on error or a closed peer. */
int net_write_all(int fd, const void *buf, size_t len);

/* net_write_all of a NUL-terminated string (its strlen bytes, sans NUL). */
int net_write_str(int fd, const char *s);

/* Read exactly `len` bytes, retrying EINTR. Returns 0 on success, -1 on error or
 * EOF before `len` bytes. */
int net_read_full(int fd, void *buf, size_t len);

/* Read a single '\n'-terminated line one byte at a time — so bytes after the
 * newline stay unread, which is required when a binary stream follows the line
 * (e.g. the replication handshake). Writes a NUL-terminated string (newline
 * stripped) of up to cap-1 bytes into `buf`. If `deadline_ms` is non-zero the
 * whole read is bounded by that CLOCK_MONOTONIC deadline (see net_mono_ms),
 * defeating a slow-loris that drips bytes under a per-recv timeout. Returns the
 * line length, or -1 on error/EOF/deadline. */
int net_read_line(int fd, char *buf, size_t cap, uint64_t deadline_ms);

/* Milliseconds from CLOCK_MONOTONIC; use to build net_read_line deadlines. */
uint64_t net_mono_ms(void);

#endif /* AEGISDB_NETIO_H */