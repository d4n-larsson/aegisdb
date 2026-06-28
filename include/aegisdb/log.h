/* Append-only log with a self-describing, corruption-resilient frame format.
 *
 * Frame layout on disk (v2):
 *   [MAGIC: u32 LE][LEN: u32 LE][PAYLOAD_CRC: u32 LE][HEADER_CRC: u32 LE]
 *   [PAYLOAD: LEN bytes]
 *
 * MAGIC is a fixed sync marker that lets recovery resynchronize to the next
 * frame after damage. HEADER_CRC covers the first 12 bytes (magic+len+payload
 * crc), so a corrupt length is detected as a header fault distinct from a
 * payload fault. PAYLOAD_CRC covers the payload. Because the header is
 * checksummed independently, a single damaged frame in the middle of the log no
 * longer forces the entire tail to be discarded: the scanner skips the bad
 * frame and recovers the good frames that follow.
 *
 * The MAGIC also versions the format: a legacy v1 log (8-byte [CRC][LEN] header,
 * no magic) is detected on open and migrated in place to v2. */
#ifndef AEGISDB_LOG_H
#define AEGISDB_LOG_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define LOG_FRAME_HEADER 16 /* magic(4) + len(4) + payload_crc(4) + hdr_crc(4) */

typedef struct {
    int fd;
    char path[1100];
    off_t size;             /* current valid end offset (bytes) */
    pthread_mutex_t wlock;  /* serializes appends */
    /* atomic: mutated under wlock, but log_flush_pending reads it lock-free. */
    _Atomic size_t since_fsync;
    size_t fsync_batch;     /* fsync after this many appends (0 = every append) */
} LogFile;

/* Open (creating if needed) the log at `path`. A legacy v1 log is migrated to
 * the v2 frame format in place before returning. Returns 0/-1. */
int log_open(LogFile *lf, const char *path, size_t fsync_batch);
void log_close(LogFile *lf);

/* Append a payload frame. On success sets *out_offset to the frame start and
 * returns 0. Triggers batched fsync per fsync_batch. */
int log_append(LogFile *lf, const uint8_t *payload, size_t len,
               uint64_t *out_offset);

/* Read the payload of the frame at `offset`. Allocates *out (free with free()),
 * verifies the header and payload CRCs. Returns 0/-1. */
int log_read(LogFile *lf, uint64_t offset, uint8_t **out, size_t *out_len);

/* Force durability: fsync and reset the unflushed-append counter. */
void log_fsync(LogFile *lf);

/* True if appends have happened since the last fsync (a flush would do work).
 * Lock-free hint for the maintenance thread's interval flush. */
int log_flush_pending(const LogFile *lf);

/* Truncate the on-disk log to `valid_end` bytes (drops a torn tail). */
int log_truncate(LogFile *lf, uint64_t valid_end);

/* Result of a full scan, describing what recovery should do. */
typedef struct {
    uint64_t truncate_to;     /* offset recovery should truncate the log to */
    size_t good_frames;       /* frames delivered to the callback */
    size_t corrupt_frames;    /* frames skipped due to magic/CRC corruption */
    int recovered_after_hole; /* a good frame followed a corrupt region: the log
                               * has a mid-stream hole, not just a torn tail */
} LogScanResult;

/* Scan frames from byte offset `start`, invoking cb(offset,payload,len,ctx) for
 * each intact frame. `start` must be a frame boundary (0, or a checkpoint's
 * covered offset); the region before it is assumed clean. On encountering a
 * damaged frame the scanner resynchronizes to the next valid frame rather than
 * stopping. A trailing incomplete frame (torn tail from a mid-write crash) sets
 * truncate_to to the end of the last clean frame; mid-stream corruption that is
 * followed by recoverable frames leaves truncate_to at the file size so the good
 * tail is preserved. cb returning non-zero aborts the scan early. Returns 0/-1. */
typedef int (*log_scan_cb)(uint64_t offset, const uint8_t *payload, size_t len,
                           void *ctx);
int log_scan(LogFile *lf, uint64_t start, log_scan_cb cb, void *ctx,
             LogScanResult *out);

#endif /* AEGISDB_LOG_H */