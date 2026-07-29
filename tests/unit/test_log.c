/* Unit tests for the append-only log: framing, read-back, scan, torn tail. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aegisdb/crc32.h"
#include "aegisdb/log.h"
#include "unity.h"

static char g_path[256];

void setUp(void) {
    snprintf(g_path, sizeof(g_path), "/tmp/aegis_test_log_%d.log",
             (int)getpid());
    remove(g_path);
}
void tearDown(void) { remove(g_path); }

static void test_append_read_roundtrip(void) {
    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL));

    const char *a = "first frame";
    const char *b = "second, longer frame payload";
    uint64_t off_a = 0, off_b = 0;
    TEST_ASSERT_EQUAL_INT(
        0, log_append(&lf, (const uint8_t *)a, strlen(a), &off_a));
    TEST_ASSERT_EQUAL_INT(
        0, log_append(&lf, (const uint8_t *)b, strlen(b), &off_b));
    TEST_ASSERT_EQUAL_UINT64(0, off_a);
    TEST_ASSERT_EQUAL_UINT64(LOG_FRAME_HEADER + strlen(a), off_b);

    uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL_INT(0, log_read(&lf, off_a, &out, &out_len));
    TEST_ASSERT_EQUAL_size_t(strlen(a), out_len);
    TEST_ASSERT_EQUAL_MEMORY(a, out, out_len);
    free(out);

    TEST_ASSERT_EQUAL_INT(0, log_read(&lf, off_b, &out, &out_len));
    TEST_ASSERT_EQUAL_size_t(strlen(b), out_len);
    TEST_ASSERT_EQUAL_MEMORY(b, out, out_len);
    free(out);

    log_close(&lf);
}

static int count_cb(uint64_t offset, const uint8_t *payload, size_t len,
                    void *ctx) {
    (void)offset;
    (void)payload;
    (void)len;
    (*(int *)ctx)++;
    return 0;
}

static void test_scan_visits_all_frames(void) {
    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL));
    for (int i = 0; i < 5; i++) {
        uint64_t off;
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "rec%d", i);
        log_append(&lf, (const uint8_t *)tmp, strlen(tmp), &off);
    }
    int n = 0;
    LogScanResult res = {0};
    TEST_ASSERT_EQUAL_INT(
        0, log_scan(&lf, 0, (uint64_t)lf.size, count_cb, &n, &res));
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_EQUAL_size_t(5, res.good_frames);
    TEST_ASSERT_EQUAL_size_t(0, res.corrupt_frames);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)lf.size, res.truncate_to);
    log_close(&lf);
}

/* Persistence across reopen: data must survive a close/open cycle. */
static void test_reopen_persists(void) {
    uint64_t off;
    {
        LogFile lf;
        TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL));
        log_append(&lf, (const uint8_t *)"durable", 7, &off);
        log_fsync(&lf);
        log_close(&lf);
    }
    LogFile lf2;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf2, g_path, 0, NULL, NULL));
    uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL_INT(0, log_read(&lf2, off, &out, &out_len));
    TEST_ASSERT_EQUAL_size_t(7, out_len);
    TEST_ASSERT_EQUAL_MEMORY("durable", out, out_len);
    free(out);
    log_close(&lf2);
}

/* A garbage byte appended after a valid frame (simulated torn write) must be
 * detected by scan, which reports the valid end at the good frame boundary. */
static void test_torn_tail_detected(void) {
    uint64_t good_end;
    {
        LogFile lf;
        TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL));
        uint64_t off;
        log_append(&lf, (const uint8_t *)"complete", 8, &off);
        good_end = (uint64_t)lf.size;
        log_close(&lf);
    }
    /* Append a partial/garbled frame header directly to the file. */
    FILE *f = fopen(g_path, "ab");
    TEST_ASSERT_NOT_NULL(f);
    unsigned char junk[6] = {0xFF, 0x00, 0x10, 0x00, 0x00, 0x00};
    fwrite(junk, 1, sizeof(junk), f);
    fclose(f);

    LogFile lf2;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf2, g_path, 0, NULL, NULL));
    int n = 0;
    LogScanResult res = {0};
    TEST_ASSERT_EQUAL_INT(
        0, log_scan(&lf2, 0, (uint64_t)lf2.size, count_cb, &n, &res));
    TEST_ASSERT_EQUAL_INT(1, n); /* only the complete frame */
    TEST_ASSERT_EQUAL_UINT64(good_end, res.truncate_to);
    log_close(&lf2);
}

/* Flip a byte inside the FIRST of three frames. Recovery must skip the damaged
 * frame and still recover the two that follow it (no whole-tail truncation). */
static void test_midlog_corruption_recovers_tail(void) {
    uint64_t off[3];
    uint64_t payload_start;
    {
        LogFile lf;
        TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL));
        log_append(&lf, (const uint8_t *)"alpha", 5, &off[0]);
        log_append(&lf, (const uint8_t *)"bravo", 5, &off[1]);
        log_append(&lf, (const uint8_t *)"charlie", 7, &off[2]);
        payload_start = off[0] + LOG_FRAME_HEADER; /* first frame's payload */
        log_close(&lf);
    }
    /* Corrupt a payload byte of frame 0 (header CRC stays valid, payload CRC
     * fails) so the scanner skips exactly that frame by its trusted length. */
    int fd = open(g_path, O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);
    uint8_t b = 0;
    TEST_ASSERT_EQUAL_INT(1, pread(fd, &b, 1, (off_t)payload_start));
    b ^= 0xFF;
    TEST_ASSERT_EQUAL_INT(1, pwrite(fd, &b, 1, (off_t)payload_start));
    close(fd);

    LogFile lf2;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf2, g_path, 0, NULL, NULL));
    int n = 0;
    LogScanResult res = {0};
    TEST_ASSERT_EQUAL_INT(
        0, log_scan(&lf2, 0, (uint64_t)lf2.size, count_cb, &n, &res));
    TEST_ASSERT_EQUAL_INT(2, n); /* bravo + charlie survive */
    TEST_ASSERT_EQUAL_size_t(2, res.good_frames);
    TEST_ASSERT_EQUAL_size_t(1, res.corrupt_frames);
    TEST_ASSERT_TRUE(res.recovered_after_hole);
    /* The good tail is preserved, not truncated. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)lf2.size, res.truncate_to);
    log_close(&lf2);
}

/* A legacy v1 log (8-byte [crc][len] frames, no magic) is migrated on open and
 * its records read back through the v2 path. */
static void test_legacy_v1_migration(void) {
    /* Hand-write two v1 frames: [crc32(payload) u32 LE][len u32 LE][payload]. */
    const char *p0 = "legacy-one";
    const char *p1 = "legacy-two-longer";
    int fd = open(g_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT_TRUE(fd >= 0);
    off_t at = 0;
    const char *payloads[2] = {p0, p1};
    for (int i = 0; i < 2; i++) {
        size_t len = strlen(payloads[i]);
        uint8_t h[8];
        uint32_t crc = crc32_compute((const uint8_t *)payloads[i], len);
        for (int k = 0; k < 4; k++)
            h[k] = (uint8_t)(crc >> (8 * k));
        for (int k = 0; k < 4; k++)
            h[4 + k] = (uint8_t)(len >> (8 * k));
        TEST_ASSERT_EQUAL_INT(8, pwrite(fd, h, 8, at));
        at += 8;
        TEST_ASSERT_EQUAL_INT((int)len, pwrite(fd, payloads[i], len, at));
        at += (off_t)len;
    }
    close(fd);

    LogFile lf;
    TEST_ASSERT_EQUAL_INT(
        0, log_open(&lf, g_path, 0, NULL, NULL)); /* triggers migration */
    int n = 0;
    LogScanResult res = {0};
    TEST_ASSERT_EQUAL_INT(
        0, log_scan(&lf, 0, (uint64_t)lf.size, count_cb, &n, &res));
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_size_t(0, res.corrupt_frames);

    uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL_INT(0, log_read(&lf, 0, &out, &out_len));
    TEST_ASSERT_EQUAL_size_t(strlen(p0), out_len);
    TEST_ASSERT_EQUAL_MEMORY(p0, out, out_len);
    free(out);
    log_close(&lf);
}

/* A non-empty pre-v2 log with no recoverable v1 frames (corrupt head) must NOT
 * be replaced by an empty migrated file — open fails and the original is kept. */
static void test_legacy_migration_preserves_corrupt_head(void) {
    /* v1-looking header claiming a huge payload that isn't there -> 0 frames.
     * First 4 bytes (0x11111111) are not the v2 magic, so migration is attempted. */
    uint8_t junk[12] = {0x11, 0x11, 0x11, 0x11, 0xFF, 0xFF,
                        0xFF, 0xFF, 0,    0,    0,    0};
    int fd = open(g_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(12, pwrite(fd, junk, sizeof(junk), 0));
    close(fd);

    LogFile lf;
    TEST_ASSERT_EQUAL_INT(
        -1, log_open(&lf, g_path, 0, NULL, NULL)); /* refuses to migrate */

    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(g_path, &st));
    TEST_ASSERT_EQUAL_INT(12, (int)st.st_size); /* original bytes preserved */
}

/* ---- header damage: byte-scan resynchronization ------------------------- */
/* The tests above damage a PAYLOAD, which leaves the header (and thus the frame
 * length) trusted, so the scanner steps over the bad frame by arithmetic. The
 * tests below damage the HEADER itself: the length is then untrustworthy, and
 * recovery must resynchronize by scanning forward for the next frame's magic.
 * That is a separate code path, and the one a real torn/garbled write hits. */

static void append_three(uint64_t off[3]) {
    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(
        0, log_append(&lf, (const uint8_t *)"alpha", 5, &off[0]));
    TEST_ASSERT_EQUAL_INT(
        0, log_append(&lf, (const uint8_t *)"bravo", 5, &off[1]));
    TEST_ASSERT_EQUAL_INT(
        0, log_append(&lf, (const uint8_t *)"charlie", 7, &off[2]));
    TEST_ASSERT_EQUAL_INT(0, log_close(&lf));
}

/* XOR `n` bytes at `at` in the log file, corrupting them in place. */
static void flip_bytes(off_t at, size_t n) {
    int fd = open(g_path, O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);
    for (size_t i = 0; i < n; i++) {
        uint8_t b = 0;
        TEST_ASSERT_EQUAL_INT(1, pread(fd, &b, 1, at + (off_t)i));
        b ^= 0xFF;
        TEST_ASSERT_EQUAL_INT(1, pwrite(fd, &b, 1, at + (off_t)i));
    }
    close(fd);
}

/* Collect the payloads a scan delivers, so we can assert exactly WHICH frames
 * survived rather than just how many. */
typedef struct {
    char seen[8][32];
    int n;
} Seen;

static int collect_cb(uint64_t offset, const uint8_t *payload, size_t len,
                      void *ctx) {
    (void)offset;
    Seen *s = ctx;
    if (s->n < 8 && len < sizeof(s->seen[0])) {
        memcpy(s->seen[s->n], payload, len);
        s->seen[s->n][len] = '\0';
        s->n++;
    }
    return 0;
}

/* Destroy the LENGTH field of the MIDDLE frame. Its header CRC no longer
 * matches, so the frame length cannot be trusted to step over it; the scanner
 * must byte-scan forward to the third frame's magic and recover it. */
static void test_corrupt_header_length_resyncs(void) {
    uint64_t off[3];
    append_three(off);
    flip_bytes((off_t)off[1] + 4, 4); /* len field of frame 1 */

    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL));
    Seen seen = {{{0}}, 0};
    LogScanResult res = {0};
    TEST_ASSERT_EQUAL_INT(
        0, log_scan(&lf, 0, (uint64_t)lf.size, collect_cb, &seen, &res));
    TEST_ASSERT_EQUAL_INT(2, seen.n);
    TEST_ASSERT_EQUAL_STRING("alpha", seen.seen[0]);
    TEST_ASSERT_EQUAL_STRING("charlie", seen.seen[1]); /* found by resync */
    TEST_ASSERT_EQUAL_size_t(2, res.good_frames);
    TEST_ASSERT_EQUAL_size_t(1, res.corrupt_frames);
    TEST_ASSERT_TRUE(res.recovered_after_hole);
    /* A frame was recovered past the hole, so the tail must be kept. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)lf.size, res.truncate_to);
    log_close(&lf);
}

/* Destroy the MAGIC of the middle frame: the sync marker itself is gone, so the
 * scanner cannot even recognize a frame there and must resync past it. */
static void test_corrupt_header_magic_resyncs(void) {
    uint64_t off[3];
    append_three(off);
    flip_bytes((off_t)off[1], 4); /* magic of frame 1 */

    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL));
    Seen seen = {{{0}}, 0};
    LogScanResult res = {0};
    TEST_ASSERT_EQUAL_INT(
        0, log_scan(&lf, 0, (uint64_t)lf.size, collect_cb, &seen, &res));
    TEST_ASSERT_EQUAL_INT(2, seen.n);
    TEST_ASSERT_EQUAL_STRING("alpha", seen.seen[0]);
    TEST_ASSERT_EQUAL_STRING("charlie", seen.seen[1]);
    TEST_ASSERT_EQUAL_size_t(1, res.corrupt_frames);
    TEST_ASSERT_TRUE(res.recovered_after_hole);
    log_close(&lf);
}

/* Damage the header of the LAST frame. There is nothing recoverable ahead, so
 * the resync scan runs to the end and the scanner reports a torn tail — truncate
 * back to the end of the last clean frame, keeping the two good frames. */
static void test_corrupt_header_in_last_frame_is_torn_tail(void) {
    uint64_t off[3];
    append_three(off);
    flip_bytes((off_t)off[2] + 4, 4); /* len field of the final frame */

    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL));
    Seen seen = {{{0}}, 0};
    LogScanResult res = {0};
    TEST_ASSERT_EQUAL_INT(
        0, log_scan(&lf, 0, (uint64_t)lf.size, collect_cb, &seen, &res));
    TEST_ASSERT_EQUAL_INT(2, seen.n);
    TEST_ASSERT_EQUAL_STRING("alpha", seen.seen[0]);
    TEST_ASSERT_EQUAL_STRING("bravo", seen.seen[1]);
    TEST_ASSERT_FALSE(res.recovered_after_hole);
    TEST_ASSERT_EQUAL_UINT64(off[2], res.truncate_to);
    log_close(&lf);
}

/* Damage the header of the FIRST frame: the scan starts on a bad header with no
 * preceding clean region, so recovery still finds the frames that follow. */
static void test_corrupt_header_in_first_frame_resyncs(void) {
    uint64_t off[3];
    append_three(off);
    flip_bytes((off_t)off[0] + 4, 4);

    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL));
    Seen seen = {{{0}}, 0};
    LogScanResult res = {0};
    TEST_ASSERT_EQUAL_INT(
        0, log_scan(&lf, 0, (uint64_t)lf.size, collect_cb, &seen, &res));
    TEST_ASSERT_EQUAL_INT(2, seen.n);
    TEST_ASSERT_EQUAL_STRING("bravo", seen.seen[0]);
    TEST_ASSERT_EQUAL_STRING("charlie", seen.seen[1]);
    TEST_ASSERT_EQUAL_size_t(1, res.corrupt_frames);
    TEST_ASSERT_TRUE(res.recovered_after_hole);
    log_close(&lf);
}

/* Garbage in place of a whole frame (no magic anywhere in it) must not be
 * mistaken for data: the scanner resyncs to the next real frame and reports
 * exactly one corrupt region, however many bytes it spans. */
static void test_garbage_region_resyncs_once(void) {
    uint64_t off[3];
    append_three(off);
    /* Overwrite frame 1 entirely (header + payload) with a non-magic pattern. */
    int fd = open(g_path, O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);
    uint8_t junk[LOG_FRAME_HEADER + 5];
    memset(junk, 0x5A, sizeof(junk));
    TEST_ASSERT_EQUAL_INT((int)sizeof(junk),
                          pwrite(fd, junk, sizeof(junk), (off_t)off[1]));
    close(fd);

    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL));
    Seen seen = {{{0}}, 0};
    LogScanResult res = {0};
    TEST_ASSERT_EQUAL_INT(
        0, log_scan(&lf, 0, (uint64_t)lf.size, collect_cb, &seen, &res));
    TEST_ASSERT_EQUAL_INT(2, seen.n);
    TEST_ASSERT_EQUAL_STRING("alpha", seen.seen[0]);
    TEST_ASSERT_EQUAL_STRING("charlie", seen.seen[1]);
    TEST_ASSERT_EQUAL_size_t(1, res.corrupt_frames);
    log_close(&lf);
}

/* ---- log_truncate ------------------------------------------------------- */

/* Truncation is how recovery drops a torn tail. It must shrink both the file and
 * the in-memory size, and the dropped region must be gone after a reopen. */
static void test_truncate_drops_tail(void) {
    uint64_t off[3];
    append_three(off);

    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, log_truncate(&lf, off[1])); /* keep frame 0 only */
    TEST_ASSERT_EQUAL_INT((int)off[1], (int)lf.size);
    TEST_ASSERT_EQUAL_INT(0, log_close(&lf));

    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(g_path, &st));
    TEST_ASSERT_EQUAL_INT((int)off[1], (int)st.st_size);

    LogFile lf2;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf2, g_path, 0, NULL, NULL));
    int n = 0;
    LogScanResult res = {0};
    TEST_ASSERT_EQUAL_INT(
        0, log_scan(&lf2, 0, (uint64_t)lf2.size, count_cb, &n, &res));
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_size_t(0, res.corrupt_frames);
    /* Appends resume at the truncated end rather than the old one. */
    uint64_t new_off = 0;
    TEST_ASSERT_EQUAL_INT(
        0, log_append(&lf2, (const uint8_t *)"delta", 5, &new_off));
    TEST_ASSERT_EQUAL_UINT64(off[1], new_off);
    log_close(&lf2);
}

/* Truncating to 0 empties the log without invalidating the handle. */
static void test_truncate_to_empty(void) {
    uint64_t off[3];
    append_three(off);

    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, log_truncate(&lf, 0));
    TEST_ASSERT_EQUAL_INT(0, (int)lf.size);
    int n = 0;
    TEST_ASSERT_EQUAL_INT(0, log_scan(&lf, 0, 0, count_cb, &n, NULL));
    TEST_ASSERT_EQUAL_INT(0, n);
    uint64_t new_off = 1;
    TEST_ASSERT_EQUAL_INT(
        0, log_append(&lf, (const uint8_t *)"fresh", 5, &new_off));
    TEST_ASSERT_EQUAL_UINT64(0, new_off);
    log_close(&lf);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_append_read_roundtrip);
    RUN_TEST(test_scan_visits_all_frames);
    RUN_TEST(test_reopen_persists);
    RUN_TEST(test_torn_tail_detected);
    RUN_TEST(test_midlog_corruption_recovers_tail);
    RUN_TEST(test_corrupt_header_length_resyncs);
    RUN_TEST(test_corrupt_header_magic_resyncs);
    RUN_TEST(test_corrupt_header_in_last_frame_is_torn_tail);
    RUN_TEST(test_corrupt_header_in_first_frame_resyncs);
    RUN_TEST(test_garbage_region_resyncs_once);
    RUN_TEST(test_truncate_drops_tail);
    RUN_TEST(test_truncate_to_empty);
    RUN_TEST(test_legacy_v1_migration);
    RUN_TEST(test_legacy_migration_preserves_corrupt_head);
    return UNITY_END();
}