/* Durable-write helpers: fs_write_atomic (atomic tmp+fsync+rename, exact mode,
 * overwrite, no leftover temp) and fs_copy_file (byte-for-byte copy). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aegisdb/fsutil.h"
#include "unity.h"

static char g_dir[512];

void setUp(void) {
    snprintf(g_dir, sizeof(g_dir), "/tmp/aegis_fsutil_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(g_dir));
}

void tearDown(void) {
    /* Best-effort cleanup of anything the tests wrote. */
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    int rc = system(cmd);
    (void)rc; /* best-effort cleanup; failure doesn't fail the test */
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    if (len) *len = got;
    return buf;
}

static void path_in(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", g_dir, name);
}

static void test_write_atomic_content_and_mode(void) {
    char p[600];
    path_in(p, sizeof(p), "meta");
    const char *data = "hello\0world"; /* embedded NUL: binary-safe */
    TEST_ASSERT_EQUAL_INT(0, fs_write_atomic(p, data, 11, 0600));

    size_t len = 0;
    char *got = slurp(p, &len);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_size_t(11, len);
    TEST_ASSERT_EQUAL_MEMORY(data, got, 11);
    free(got);

    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(p, &st));
    TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 0777); /* exact mode, no umask window */
}

static void test_write_atomic_overwrites(void) {
    char p[600];
    path_in(p, sizeof(p), "over");
    TEST_ASSERT_EQUAL_INT(0, fs_write_atomic(p, "aaaaaa", 6, 0644));
    TEST_ASSERT_EQUAL_INT(0, fs_write_atomic(p, "bb", 2, 0644));

    size_t len = 0;
    char *got = slurp(p, &len);
    TEST_ASSERT_EQUAL_size_t(2, len); /* replaced, not appended */
    TEST_ASSERT_EQUAL_MEMORY("bb", got, 2);
    free(got);
}

static void test_write_atomic_leaves_no_temp(void) {
    char p[600], tmp[608];
    path_in(p, sizeof(p), "notemp");
    TEST_ASSERT_EQUAL_INT(0, fs_write_atomic(p, "x", 1, 0600));
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    struct stat st;
    TEST_ASSERT_NOT_EQUAL(0, stat(tmp, &st)); /* the .tmp is gone after rename */
}

static void test_write_atomic_empty(void) {
    char p[600];
    path_in(p, sizeof(p), "empty");
    TEST_ASSERT_EQUAL_INT(0, fs_write_atomic(p, "", 0, 0600));
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(p, &st));
    TEST_ASSERT_EQUAL_INT(0, (int)st.st_size);
}

static void test_copy_file_roundtrip(void) {
    char src[600], dst[600];
    path_in(src, sizeof(src), "src.bin");
    path_in(dst, sizeof(dst), "dst.bin");
    char payload[4096];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (char)(i * 31 + 7);
    TEST_ASSERT_EQUAL_INT(0, fs_write_atomic(src, payload, sizeof(payload), 0644));

    TEST_ASSERT_EQUAL_INT(0, fs_copy_file(src, dst));
    size_t len = 0;
    char *got = slurp(dst, &len);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_size_t(sizeof(payload), len);
    TEST_ASSERT_EQUAL_MEMORY(payload, got, sizeof(payload));
    free(got);
}

static void test_copy_file_missing_src(void) {
    char src[600], dst[600];
    path_in(src, sizeof(src), "nope");
    path_in(dst, sizeof(dst), "dst2");
    TEST_ASSERT_EQUAL_INT(-1, fs_copy_file(src, dst));
    struct stat st;
    TEST_ASSERT_NOT_EQUAL(0, stat(dst, &st)); /* no partial destination left */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_write_atomic_content_and_mode);
    RUN_TEST(test_write_atomic_overwrites);
    RUN_TEST(test_write_atomic_leaves_no_temp);
    RUN_TEST(test_write_atomic_empty);
    RUN_TEST(test_copy_file_roundtrip);
    RUN_TEST(test_copy_file_missing_src);
    return UNITY_END();
}