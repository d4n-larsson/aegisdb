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
    snprintf(g_path, sizeof(g_path), "/tmp/aegis_test_log_%d.log", (int)getpid());
    remove(g_path);
}
void tearDown(void) { remove(g_path); }

static void test_append_read_roundtrip(void) {
    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL));

    const char *a = "first frame";
    const char *b = "second, longer frame payload";
    uint64_t off_a = 0, off_b = 0;
    TEST_ASSERT_EQUAL_INT(0, log_append(&lf, (const uint8_t *)a, strlen(a), &off_a));
    TEST_ASSERT_EQUAL_INT(0, log_append(&lf, (const uint8_t *)b, strlen(b), &off_b));
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

static int count_cb(uint64_t offset, const uint8_t *payload, size_t len, void *ctx) {
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
    TEST_ASSERT_EQUAL_INT(0, log_scan(&lf, 0, count_cb, &n, &res));
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
    TEST_ASSERT_EQUAL_INT(0, log_scan(&lf2, 0, count_cb, &n, &res));
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
    TEST_ASSERT_EQUAL_INT(0, log_scan(&lf2, 0, count_cb, &n, &res));
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
        for (int k = 0; k < 4; k++) h[k] = (uint8_t)(crc >> (8 * k));
        for (int k = 0; k < 4; k++) h[4 + k] = (uint8_t)(len >> (8 * k));
        TEST_ASSERT_EQUAL_INT(8, pwrite(fd, h, 8, at));
        at += 8;
        TEST_ASSERT_EQUAL_INT((int)len, pwrite(fd, payloads[i], len, at));
        at += (off_t)len;
    }
    close(fd);

    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL)); /* triggers migration */
    int n = 0;
    LogScanResult res = {0};
    TEST_ASSERT_EQUAL_INT(0, log_scan(&lf, 0, count_cb, &n, &res));
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
    uint8_t junk[12] = {0x11, 0x11, 0x11, 0x11, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
    int fd = open(g_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(12, pwrite(fd, junk, sizeof(junk), 0));
    close(fd);

    LogFile lf;
    TEST_ASSERT_EQUAL_INT(-1, log_open(&lf, g_path, 0, NULL, NULL)); /* refuses to migrate */

    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(g_path, &st));
    TEST_ASSERT_EQUAL_INT(12, (int)st.st_size); /* original bytes preserved */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_append_read_roundtrip);
    RUN_TEST(test_scan_visits_all_frames);
    RUN_TEST(test_reopen_persists);
    RUN_TEST(test_torn_tail_detected);
    RUN_TEST(test_midlog_corruption_recovers_tail);
    RUN_TEST(test_legacy_v1_migration);
    RUN_TEST(test_legacy_migration_preserves_corrupt_head);
    return UNITY_END();
}