/* Unit tests for encryption at rest at the log layer: encrypted append/read/scan
 * round-trip, key/mode reconciliation on reopen, tamper detection, and an
 * on-disk confidentiality check. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aegisdb/log.h"
#include "unity.h"

static char g_path[256];
static uint8_t KEY_A[AEAD_KEY_LEN];
static uint8_t KEY_B[AEAD_KEY_LEN];

void setUp(void) {
    snprintf(g_path, sizeof(g_path), "/tmp/aegis_test_logenc_%d.log", (int)getpid());
    remove(g_path);
    for (int i = 0; i < AEAD_KEY_LEN; i++) {
        KEY_A[i] = (uint8_t)i;
        KEY_B[i] = (uint8_t)(0xFF - i);
    }
}
void tearDown(void) { remove(g_path); }

static int scan_count(uint64_t off, const uint8_t *pay, size_t len, void *ctx) {
    (void)off;
    (void)pay;
    (void)len;
    (*(int *)ctx)++;
    return 0;
}

static void test_encrypted_roundtrip_and_scan(void) {
    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, KEY_A, NULL));
    TEST_ASSERT_EQUAL_INT(1, lf.encrypted);

    const char *a = "secret memory one";
    const char *b = "a considerably longer secret memory payload #2";
    uint64_t off_a = 0, off_b = 0;
    TEST_ASSERT_EQUAL_INT(0, log_append(&lf, (const uint8_t *)a, strlen(a), &off_a));
    TEST_ASSERT_EQUAL_INT(0, log_append(&lf, (const uint8_t *)b, strlen(b), &off_b));
    TEST_ASSERT_EQUAL_UINT64(0, off_a);
    TEST_ASSERT_NOT_EQUAL(off_a, off_b);

    uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL_INT(0, log_read(&lf, off_a, &out, &out_len));
    TEST_ASSERT_EQUAL_size_t(strlen(a), out_len);
    TEST_ASSERT_EQUAL_MEMORY(a, out, out_len);
    free(out);
    TEST_ASSERT_EQUAL_INT(0, log_read(&lf, off_b, &out, &out_len));
    TEST_ASSERT_EQUAL_MEMORY(b, out, out_len);
    free(out);

    int n = 0;
    LogScanResult res;
    TEST_ASSERT_EQUAL_INT(0, log_scan(&lf, 0, scan_count, &n, &res));
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_size_t(2, res.good_frames);
    TEST_ASSERT_EQUAL_size_t(0, res.corrupt_frames);
    log_close(&lf);
}

static void test_plaintext_not_on_disk(void) {
    LogFile lf;
    const char *secret = "SUNSCREEN-MARKER-9x";
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, KEY_A, NULL));
    TEST_ASSERT_EQUAL_INT(
        0, log_append(&lf, (const uint8_t *)secret, strlen(secret), NULL));
    log_close(&lf);

    /* The marker must not appear in cleartext anywhere in the file. */
    FILE *f = fopen(g_path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    uint8_t buf[4096];
    size_t got = fread(buf, 1, sizeof buf, f);
    fclose(f);
    int found = 0;
    size_t slen = strlen(secret);
    for (size_t i = 0; i + slen <= got; i++)
        if (memcmp(buf + i, secret, slen) == 0) { found = 1; break; }
    TEST_ASSERT_FALSE(found);
}

static void test_reopen_right_key(void) {
    LogFile lf;
    const char *a = "persisted secret";
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, KEY_A, NULL));
    TEST_ASSERT_EQUAL_INT(0, log_append(&lf, (const uint8_t *)a, strlen(a), NULL));
    log_close(&lf);

    LogFile lf2;
    LogOpenStatus st = LOG_OPEN_ERR_IO;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf2, g_path, 0, KEY_A, &st));
    TEST_ASSERT_EQUAL_INT(LOG_OPEN_OK, st);
    uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL_INT(0, log_read(&lf2, 0, &out, &out_len));
    TEST_ASSERT_EQUAL_MEMORY(a, out, out_len);
    free(out);
    log_close(&lf2);
}

static void test_reopen_wrong_key_refused(void) {
    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, KEY_A, NULL));
    TEST_ASSERT_EQUAL_INT(0, log_append(&lf, (const uint8_t *)"x", 1, NULL));
    log_close(&lf);

    LogFile lf2;
    LogOpenStatus st = LOG_OPEN_OK;
    TEST_ASSERT_EQUAL_INT(-1, log_open(&lf2, g_path, 0, KEY_B, &st));
    TEST_ASSERT_EQUAL_INT(LOG_OPEN_ERR_WRONG_KEY, st);
}

static void test_reopen_encrypted_without_key_refused(void) {
    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, KEY_A, NULL));
    TEST_ASSERT_EQUAL_INT(0, log_append(&lf, (const uint8_t *)"x", 1, NULL));
    log_close(&lf);

    LogFile lf2;
    LogOpenStatus st = LOG_OPEN_OK;
    TEST_ASSERT_EQUAL_INT(-1, log_open(&lf2, g_path, 0, NULL, &st));
    TEST_ASSERT_EQUAL_INT(LOG_OPEN_ERR_PLAIN_ON_ENC, st);
}

static void test_key_on_plaintext_log_refused(void) {
    LogFile lf;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, NULL, NULL)); /* plaintext */
    TEST_ASSERT_EQUAL_INT(0, log_append(&lf, (const uint8_t *)"hi", 2, NULL));
    log_close(&lf);

    LogFile lf2;
    LogOpenStatus st = LOG_OPEN_OK;
    TEST_ASSERT_EQUAL_INT(-1, log_open(&lf2, g_path, 0, KEY_A, &st));
    TEST_ASSERT_EQUAL_INT(LOG_OPEN_ERR_KEY_ON_PLAIN, st);
}

static void test_tamper_detected_on_read_and_scan(void) {
    LogFile lf;
    const char *a = "first frame stays intact";
    const char *b = "second frame gets tampered";
    uint64_t off2 = 0;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf, g_path, 0, KEY_A, NULL));
    TEST_ASSERT_EQUAL_INT(0, log_append(&lf, (const uint8_t *)a, strlen(a), NULL));
    TEST_ASSERT_EQUAL_INT(0, log_append(&lf, (const uint8_t *)b, strlen(b), &off2));
    log_close(&lf);

    /* Flip a byte inside the SECOND frame's ciphertext (its v3 header is 36
     * bytes). The first frame stays intact so log_open's first-frame check
     * still passes — this exercises read/scan authentication, not open. */
    int fd = open(g_path, O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);
    off_t pos = (off_t)off2 + 36 + 1;
    uint8_t byte;
    TEST_ASSERT_EQUAL_INT(1, (int)pread(fd, &byte, 1, pos));
    byte ^= 0x40;
    TEST_ASSERT_EQUAL_INT(1, (int)pwrite(fd, &byte, 1, pos));
    close(fd);

    LogFile lf2;
    LogOpenStatus st = LOG_OPEN_ERR_IO;
    TEST_ASSERT_EQUAL_INT(0, log_open(&lf2, g_path, 0, KEY_A, &st));
    TEST_ASSERT_EQUAL_INT(LOG_OPEN_OK, st); /* first frame still verifies */

    uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL_INT(0, log_read(&lf2, 0, &out, &out_len)); /* frame 1 ok */
    free(out);
    TEST_ASSERT_EQUAL_INT(-1, log_read(&lf2, off2, &out, &out_len)); /* auth fails */

    int n = 0;
    LogScanResult res;
    TEST_ASSERT_EQUAL_INT(0, log_scan(&lf2, 0, scan_count, &n, &res));
    TEST_ASSERT_EQUAL_INT(1, n);                     /* only frame 1 delivered */
    TEST_ASSERT_EQUAL_size_t(1, res.good_frames);
    TEST_ASSERT_EQUAL_size_t(1, res.corrupt_frames); /* frame 2 flagged corrupt */
    log_close(&lf2);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_encrypted_roundtrip_and_scan);
    RUN_TEST(test_plaintext_not_on_disk);
    RUN_TEST(test_reopen_right_key);
    RUN_TEST(test_reopen_wrong_key_refused);
    RUN_TEST(test_reopen_encrypted_without_key_refused);
    RUN_TEST(test_key_on_plaintext_log_refused);
    RUN_TEST(test_tamper_detected_on_read_and_scan);
    return UNITY_END();
}