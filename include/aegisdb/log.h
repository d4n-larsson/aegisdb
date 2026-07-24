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
 * no magic) is detected on open and migrated in place to v2.
 *
 * When a key is supplied (encryption at rest, see docs/encryption-at-rest-design.md)
 * frames use a v3 layout instead — a distinct magic, so the two are told apart
 * the same way v1/v2 are:
 *   [MAGIC_ENC: u32][LEN: u32][NONCE: 24B][HEADER_CRC: u32]
 *   [CIPHERTEXT: LEN bytes][TAG: 16B]
 * LEN is the plaintext length (XChaCha20 is a stream cipher, so ciphertext is
 * the same length), so offset arithmetic is unchanged apart from the larger
 * per-frame overhead. HEADER_CRC covers magic+len+nonce for keyless resync; the
 * AEAD authenticates the ciphertext with that same prefix as associated data,
 * superseding the payload CRC. A log is uniformly plaintext or encrypted. */
#ifndef AEGISDB_LOG_H
#define AEGISDB_LOG_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "aegisdb/aead.h"
#include "aegisdb/types.h"

#define LOG_FRAME_HEADER 16 /* v2: magic(4) + len(4) + payload_crc(4) + hdr_crc(4) */

typedef struct {
    int fd;
    char path[AEGIS_PATH_MAX];
    off_t size;             /* current valid end offset (bytes) */
    pthread_mutex_t wlock;  /* serializes appends */
    /* atomic: mutated under wlock, but log_flush_pending reads it lock-free. */
    _Atomic size_t since_fsync;
    size_t fsync_batch;     /* fsync after this many appends (0 = every append) */
    int encrypted;          /* 1: frames are v3 AEAD frames sealed with `key` */
    uint8_t key[AEAD_KEY_LEN]; /* frame-encryption key; valid iff encrypted */
} LogFile;

/* Reason a log failed to open, for a clear operator message (log_open logs it). */
typedef enum {
    LOG_OPEN_OK = 0,
    LOG_OPEN_ERR_IO,            /* filesystem / migration failure */
    LOG_OPEN_ERR_KEY_ON_PLAIN,  /* key supplied but the log is plaintext */
    LOG_OPEN_ERR_PLAIN_ON_ENC,  /* encrypted log but no key supplied */
    LOG_OPEN_ERR_WRONG_KEY,     /* key supplied does not decrypt the log */
} LogOpenStatus;

/* Open (creating if needed) the log at `path`. `key` (AEAD_KEY_LEN bytes) enables
 * encryption at rest; NULL leaves the log plaintext. A legacy v1 log is migrated
 * to v2 in place. The plaintext/encrypted mode of an existing log must match the
 * key argument (fail-closed) — see LogOpenStatus. Returns 0 on success, -1 on
 * failure; on failure *status (may be NULL) carries the reason. */
int log_open(LogFile *lf, const char *path, size_t fsync_batch,
             const uint8_t *key, LogOpenStatus *status);
void log_close(LogFile *lf);

/* Append a payload frame. On success sets *out_offset to the frame start and
 * returns 0. Triggers batched fsync per fsync_batch. */
int log_append(LogFile *lf, const uint8_t *payload, size_t len,
               uint64_t *out_offset);

/* Read the payload of the frame at `offset`. Allocates *out (free with free()),
 * verifies the header and payload CRCs. Returns 0/-1. */
int log_read(LogFile *lf, uint64_t offset, uint8_t **out, size_t *out_len);

/* On-disk bytes a frame occupies beyond its payload (header [+ AEAD tag]). The
 * total frame size is log_frame_overhead(lf) + payload_len; callers tailing the
 * log by offset use this to step to the next frame regardless of mode. */
size_t log_frame_overhead(const LogFile *lf);

/* Force durability: fsync and reset the unflushed-append counter. */
void log_fsync(LogFile *lf);

/* fsync only if the configured batch threshold has been reached (sync/batch
 * durability). Call after releasing the index lock so the fsync is not held
 * under it; a no-op in interval mode. */
void log_fsync_if_batched(LogFile *lf);

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