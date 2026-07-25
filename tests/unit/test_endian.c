/* Known-answer + round-trip tests for the little-endian serialization codec.
 * These pin the byte order explicitly so the codec can never regress to a
 * host-endian memcpy (which would silently corrupt data on a big-endian host). */
#include <stdint.h>
#include <string.h>

#include "aegisdb/endian.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_put_known_bytes(void) {
    uint8_t b[8];

    aegis_put_u16le(b, 0x0201);
    TEST_ASSERT_EQUAL_HEX8(0x01, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, b[1]);

    aegis_put_u32le(b, 0x04030201u);
    uint8_t want32[4] = {0x01, 0x02, 0x03, 0x04};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want32, b, 4);

    aegis_put_u64le(b, 0x0807060504030201ULL);
    uint8_t want64[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want64, b, 8);
}

static void test_get_known_bytes(void) {
    uint8_t b16[2] = {0x01, 0x02};
    TEST_ASSERT_EQUAL_HEX16(0x0201, aegis_get_u16le(b16));

    uint8_t b32[4] = {0x01, 0x02, 0x03, 0x04};
    TEST_ASSERT_EQUAL_HEX32(0x04030201u, aegis_get_u32le(b32));

    uint8_t b64[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    TEST_ASSERT_EQUAL_HEX64(0x0807060504030201ULL, aegis_get_u64le(b64));
}

static void test_roundtrip_edges(void) {
    uint8_t b[8];
    uint16_t v16[] = {0, 1, 0x00FF, 0x7FFF, 0xFFFF};
    for (size_t i = 0; i < sizeof(v16) / sizeof(*v16); i++) {
        aegis_put_u16le(b, v16[i]);
        TEST_ASSERT_EQUAL_HEX16(v16[i], aegis_get_u16le(b));
    }
    uint32_t v32[] = {0, 1, 0xFFu, 0xDEADBEEFu, 0xFFFFFFFFu};
    for (size_t i = 0; i < sizeof(v32) / sizeof(*v32); i++) {
        aegis_put_u32le(b, v32[i]);
        TEST_ASSERT_EQUAL_HEX32(v32[i], aegis_get_u32le(b));
    }
    uint64_t v64[] = {0, 1, 0xFFFFFFFFULL, 0xDEADBEEFCAFEBABEULL, ~0ULL};
    for (size_t i = 0; i < sizeof(v64) / sizeof(*v64); i++) {
        aegis_put_u64le(b, v64[i]);
        TEST_ASSERT_EQUAL_HEX64(v64[i], aegis_get_u64le(b));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_put_known_bytes);
    RUN_TEST(test_get_known_bytes);
    RUN_TEST(test_roundtrip_edges);
    return UNITY_END();
}