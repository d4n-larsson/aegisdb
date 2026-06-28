/* SHA-256 known-answer tests (FIPS 180-4 examples + edge cases). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aegisdb/sha256.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void to_hex(const uint8_t *d, char *out) {
    for (int i = 0; i < SHA256_DIGEST_LEN; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
}

static void check_vec(const char *msg, size_t len, const char *expect) {
    uint8_t d[SHA256_DIGEST_LEN];
    char hex[2 * SHA256_DIGEST_LEN + 1];
    sha256(msg, len, d);
    to_hex(d, hex);
    TEST_ASSERT_EQUAL_STRING(expect, hex);
}

static void test_empty(void) {
    check_vec("", 0,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

static void test_abc(void) {
    check_vec("abc", 3,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

/* 56 bytes: exercises the two-block padding path (len >= 56). */
static void test_two_block_padding(void) {
    check_vec("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

/* A long input crossing multiple 64-byte blocks. */
static void test_multiblock(void) {
    char buf[1000];
    memset(buf, 'a', sizeof(buf));
    check_vec(buf, sizeof(buf),
              "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_empty);
    RUN_TEST(test_abc);
    RUN_TEST(test_two_block_padding);
    RUN_TEST(test_multiblock);
    return UNITY_END();
}