/* XChaCha20-Poly1305 known-answer tests.
 *
 * Primitive vectors: ChaCha20 block (RFC 8439 §2.3.2), HChaCha20
 * (draft-irtf-cfrg-xchacha §2.2.1), Poly1305 (RFC 8439 §2.5.2). End-to-end AEAD
 * vector: XChaCha20-Poly1305 (draft-irtf-cfrg-xchacha §A.3.1). Plus round-trip,
 * tamper-detection, in-place aliasing, and empty-input edge cases. */
#include <stdint.h>
#include <string.h>

#include "aegisdb/aead.h"
#include "unity.h"

/* Primitives given external linkage in chacha20poly1305.c for these KATs. */
void aegis_chacha20_block(const uint8_t key[32], uint32_t counter,
                          const uint8_t nonce[12], uint8_t out[64]);
void aegis_hchacha20(const uint8_t key[32], const uint8_t nonce16[16],
                     uint8_t out[32]);
void aegis_poly1305(uint8_t mac[16], const uint8_t *m, size_t len,
                    const uint8_t key[32]);

void setUp(void) {}
void tearDown(void) {}

static void iota(uint8_t *b, size_t n, uint8_t start) {
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(start + i);
}

/* ---- primitive KATs ---------------------------------------------------- */

static void test_chacha20_block_rfc8439(void) {
    uint8_t key[32];
    iota(key, 32, 0);
    uint8_t nonce[12] = {0x00, 0x00, 0x00, 0x09, 0x00, 0x00,
                         0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
    uint8_t out[64];
    aegis_chacha20_block(key, 1, nonce, out);
    static const uint8_t expect[64] = {
        0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd,
        0x1f, 0xa3, 0x20, 0x71, 0xc4, 0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0,
        0x68, 0x03, 0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e, 0xd2,
        0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09, 0x14, 0xc2, 0xd7, 0x05,
        0xd9, 0x8b, 0x02, 0xa2, 0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e,
        0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e};
    TEST_ASSERT_EQUAL_MEMORY(expect, out, 64);
}

static void test_hchacha20_draft(void) {
    uint8_t key[32];
    iota(key, 32, 0);
    uint8_t nonce16[16] = {0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x4a,
                           0x00, 0x00, 0x00, 0x00, 0x31, 0x41, 0x59, 0x27};
    uint8_t out[32];
    aegis_hchacha20(key, nonce16, out);
    static const uint8_t expect[32] = {
        0x82, 0x41, 0x3b, 0x42, 0x27, 0xb2, 0x7b, 0xfe, 0xd3, 0x0e, 0x42,
        0x50, 0x8a, 0x87, 0x7d, 0x73, 0xa0, 0xf9, 0xe4, 0xd5, 0x8a, 0x74,
        0xa8, 0x53, 0xc1, 0x2e, 0xc4, 0x13, 0x26, 0xd3, 0xec, 0xdc};
    TEST_ASSERT_EQUAL_MEMORY(expect, out, 32);
}

static void test_poly1305_rfc8439(void) {
    static const uint8_t key[32] = {
        0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33, 0x7f, 0x44, 0x52,
        0xfe, 0x42, 0xd5, 0x06, 0xa8, 0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d,
        0xb2, 0xfd, 0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b};
    const char *msg = "Cryptographic Forum Research Group";
    uint8_t mac[16];
    aegis_poly1305(mac, (const uint8_t *)msg, strlen(msg), key);
    static const uint8_t expect[16] = {0xa8, 0x06, 0x1d, 0xc1, 0x30, 0x51,
                                        0x36, 0xc6, 0xc2, 0x2b, 0x8b, 0xaf,
                                        0x0c, 0x01, 0x27, 0xa9};
    TEST_ASSERT_EQUAL_MEMORY(expect, mac, 16);
}

/* ---- end-to-end AEAD KAT (draft §A.3.1) -------------------------------- */

static void test_xchacha20poly1305_draft_vector(void) {
    uint8_t key[32];
    iota(key, 32, 0x80);
    uint8_t nonce[24];
    iota(nonce, 24, 0x40);
    static const uint8_t aad[12] = {0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1,
                                     0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7};
    const char *pt =
        "Ladies and Gentlemen of the class of '99: If I could offer you only "
        "one tip for the future, sunscreen would be it.";
    size_t pt_len = strlen(pt); /* 114 */
    static const uint8_t expect_ct[114] = {
        0xbd, 0x6d, 0x17, 0x9d, 0x3e, 0x83, 0xd4, 0x3b, 0x95, 0x76, 0x57, 0x94,
        0x93, 0xc0, 0xe9, 0x39, 0x57, 0x2a, 0x17, 0x00, 0x25, 0x2b, 0xfa, 0xcc,
        0xbe, 0xd2, 0x90, 0x2c, 0x21, 0x39, 0x6c, 0xbb, 0x73, 0x1c, 0x7f, 0x1b,
        0x0b, 0x4a, 0xa6, 0x44, 0x0b, 0xf3, 0xa8, 0x2f, 0x4e, 0xda, 0x7e, 0x39,
        0xae, 0x64, 0xc6, 0x70, 0x8c, 0x54, 0xc2, 0x16, 0xcb, 0x96, 0xb7, 0x2e,
        0x12, 0x13, 0xb4, 0x52, 0x2f, 0x8c, 0x9b, 0xa4, 0x0d, 0xb5, 0xd9, 0x45,
        0xb1, 0x1b, 0x69, 0xb9, 0x82, 0xc1, 0xbb, 0x9e, 0x3f, 0x3f, 0xac, 0x2b,
        0xc3, 0x69, 0x48, 0x8f, 0x76, 0xb2, 0x38, 0x35, 0x65, 0xd3, 0xff, 0xf9,
        0x21, 0xf9, 0x66, 0x4c, 0x97, 0x63, 0x7d, 0xa9, 0x76, 0x88, 0x12, 0xf6,
        0x15, 0xc6, 0x8b, 0x13, 0xb5, 0x2e};
    static const uint8_t expect_tag[16] = {0xc0, 0x87, 0x59, 0x24, 0xc1, 0xc7,
                                           0x98, 0x79, 0x47, 0xde, 0xaf, 0xd8,
                                           0x78, 0x0a, 0xcf, 0x49};

    uint8_t ct[114], tag[16];
    aead_seal(key, nonce, aad, sizeof aad, (const uint8_t *)pt, pt_len, ct, tag);
    TEST_ASSERT_EQUAL_MEMORY(expect_ct, ct, pt_len);
    TEST_ASSERT_EQUAL_MEMORY(expect_tag, tag, 16);

    /* open() recovers the plaintext and accepts the tag */
    uint8_t back[114];
    TEST_ASSERT_EQUAL_INT(
        0, aead_open(key, nonce, aad, sizeof aad, ct, pt_len, back, tag));
    TEST_ASSERT_EQUAL_MEMORY(pt, back, pt_len);
}

/* ---- behavioral tests -------------------------------------------------- */

static void test_seal_open_roundtrip(void) {
    uint8_t key[32], nonce[24];
    iota(key, 32, 7);
    iota(nonce, 24, 200);
    const char *aad = "frame-header";
    uint8_t pt[200];
    for (size_t i = 0; i < sizeof pt; i++) pt[i] = (uint8_t)(i * 3 + 1);
    uint8_t ct[200], tag[16], back[200];
    aead_seal(key, nonce, (const uint8_t *)aad, strlen(aad), pt, sizeof pt, ct,
              tag);
    TEST_ASSERT_EQUAL_INT(0, aead_open(key, nonce, (const uint8_t *)aad,
                                       strlen(aad), ct, sizeof ct, back, tag));
    TEST_ASSERT_EQUAL_MEMORY(pt, back, sizeof pt);
}

static void test_tamper_is_detected(void) {
    uint8_t key[32], nonce[24];
    iota(key, 32, 1);
    iota(nonce, 24, 2);
    const char *aad = "aad";
    uint8_t pt[64];
    iota(pt, sizeof pt, 0);
    uint8_t ct[64], tag[16], back[64];
    aead_seal(key, nonce, (const uint8_t *)aad, 3, pt, sizeof pt, ct, tag);

    /* flip a ciphertext byte */
    ct[0] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(-1,
                          aead_open(key, nonce, (const uint8_t *)aad, 3, ct,
                                    sizeof ct, back, tag));
    ct[0] ^= 0x01; /* restore */

    /* flip a tag byte */
    uint8_t bad_tag[16];
    memcpy(bad_tag, tag, 16);
    bad_tag[15] ^= 0x80;
    TEST_ASSERT_EQUAL_INT(-1,
                          aead_open(key, nonce, (const uint8_t *)aad, 3, ct,
                                    sizeof ct, back, bad_tag));

    /* alter the AAD */
    TEST_ASSERT_EQUAL_INT(-1,
                          aead_open(key, nonce, (const uint8_t *)"AAD", 3, ct,
                                    sizeof ct, back, tag));

    /* wrong key */
    uint8_t key2[32];
    iota(key2, 32, 99);
    TEST_ASSERT_EQUAL_INT(-1,
                          aead_open(key2, nonce, (const uint8_t *)aad, 3, ct,
                                    sizeof ct, back, tag));
}

static void test_open_failure_zeroes_output(void) {
    uint8_t key[32], nonce[24];
    iota(key, 32, 5);
    iota(nonce, 24, 6);
    uint8_t pt[48];
    iota(pt, sizeof pt, 10);
    uint8_t ct[48], tag[16], back[48];
    aead_seal(key, nonce, NULL, 0, pt, sizeof pt, ct, tag);
    tag[0] ^= 0xff; /* corrupt */
    memset(back, 0xAA, sizeof back);
    TEST_ASSERT_EQUAL_INT(
        -1, aead_open(key, nonce, NULL, 0, ct, sizeof ct, back, tag));
    for (size_t i = 0; i < sizeof back; i++)
        TEST_ASSERT_EQUAL_UINT8(0, back[i]); /* no partial plaintext leaked */
}

static void test_inplace_and_empty(void) {
    uint8_t key[32], nonce[24];
    iota(key, 32, 3);
    iota(nonce, 24, 9);
    /* in-place: ct aliases pt */
    uint8_t buf[80];
    iota(buf, sizeof buf, 0);
    uint8_t orig[80];
    memcpy(orig, buf, sizeof buf);
    uint8_t tag[16];
    aead_seal(key, nonce, NULL, 0, buf, sizeof buf, buf, tag);
    TEST_ASSERT_EQUAL_INT(0,
                          aead_open(key, nonce, NULL, 0, buf, sizeof buf, buf, tag));
    TEST_ASSERT_EQUAL_MEMORY(orig, buf, sizeof buf);

    /* empty plaintext, non-empty AAD: still authenticates */
    uint8_t etag[16], edummy[1];
    aead_seal(key, nonce, (const uint8_t *)"x", 1, edummy, 0, edummy, etag);
    TEST_ASSERT_EQUAL_INT(
        0, aead_open(key, nonce, (const uint8_t *)"x", 1, edummy, 0, edummy, etag));
    TEST_ASSERT_EQUAL_INT(-1, aead_open(key, nonce, (const uint8_t *)"y", 1,
                                        edummy, 0, edummy, etag));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_chacha20_block_rfc8439);
    RUN_TEST(test_hchacha20_draft);
    RUN_TEST(test_poly1305_rfc8439);
    RUN_TEST(test_xchacha20poly1305_draft_vector);
    RUN_TEST(test_seal_open_roundtrip);
    RUN_TEST(test_tamper_is_detected);
    RUN_TEST(test_open_failure_zeroes_output);
    RUN_TEST(test_inplace_and_empty);
    return UNITY_END();
}