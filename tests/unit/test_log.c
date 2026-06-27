/* Unit tests for the append-only log: framing, read-back, scan, torn tail. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0));

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
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0));
    for (int i = 0; i < 5; i++) {
        uint64_t off;
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "rec%d", i);
        log_append(&lf, (const uint8_t *)tmp, strlen(tmp), &off);
    }
    int n = 0;
    uint64_t valid_end = 0;
    TEST_ASSERT_EQUAL_INT(0, log_scan(&lf, count_cb, &n, &valid_end));
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)lf.size, valid_end);
    log_close(&lf);
}

/* Persistence across reopen: data must survive a close/open cycle. */
static void test_reopen_persists(void) {
    uint64_t off;
    {
        LogFile lf;
        TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0));
        log_append(&lf, (const uint8_t *)"durable", 7, &off);
        log_fsync(&lf);
        log_close(&lf);
    }
    LogFile lf2;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf2, g_path, 0));
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
        TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0));
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
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf2, g_path, 0));
    int n = 0;
    uint64_t valid_end = 0;
    TEST_ASSERT_EQUAL_INT(0, log_scan(&lf2, count_cb, &n, &valid_end));
    TEST_ASSERT_EQUAL_INT(1, n); /* only the complete frame */
    TEST_ASSERT_EQUAL_UINT64(good_end, valid_end);
    log_close(&lf2);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_append_read_roundtrip);
    RUN_TEST(test_scan_visits_all_frames);
    RUN_TEST(test_reopen_persists);
    RUN_TEST(test_torn_tail_detected);
    return UNITY_END();
}