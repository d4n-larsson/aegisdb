/* Append-only log with CRC32 + length framing (T011).
 *
 * Frame layout on disk:
 *   [CRC32: u32 LE][LENGTH: u32 LE][PAYLOAD: LENGTH bytes]
 * CRC is computed over the PAYLOAD bytes only. A torn tail (short/garbled
 * trailing frame from a mid-write crash) is detected and ignored on scan. */
#ifndef AEGISDB_LOG_H
#define AEGISDB_LOG_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define LOG_FRAME_HEADER 8 /* crc(4) + len(4) */

typedef struct {
    int fd;
    char path[1100];
    off_t size;             /* current valid end offset (bytes) */
    pthread_mutex_t wlock;  /* serializes appends */
    size_t since_fsync;
    size_t fsync_batch;     /* fsync after this many appends (0 = every append) */
} LogFile;

/* Open (creating if needed) the log at `path`. Returns 0/-1. */
int log_open(LogFile *lf, const char *path, size_t fsync_batch);
void log_close(LogFile *lf);

/* Append a payload frame. On success sets *out_offset to the frame start and
 * returns 0. Triggers batched fsync per fsync_batch. */
int log_append(LogFile *lf, const uint8_t *payload, size_t len,
               uint64_t *out_offset);

/* Read the payload of the frame at `offset`. Allocates *out (free with free()),
 * verifies CRC. Returns 0/-1. */
int log_read(LogFile *lf, uint64_t offset, uint8_t **out, size_t *out_len);

/* Force durability. */
void log_fsync(LogFile *lf);

/* Truncate the on-disk log to `valid_end` bytes (drops a torn tail). */
int log_truncate(LogFile *lf, uint64_t valid_end);

/* Scan every valid frame from the start, invoking cb(offset,payload,len,ctx).
 * Stops at the first invalid/torn frame. Writes the byte offset of the first
 * invalid frame (the valid end) to *out_valid_end. cb returning non-zero
 * aborts the scan early. Returns 0/-1. */
typedef int (*log_scan_cb)(uint64_t offset, const uint8_t *payload, size_t len,
                           void *ctx);
int log_scan(LogFile *lf, log_scan_cb cb, void *ctx, uint64_t *out_valid_end);

#endif /* AEGISDB_LOG_H */