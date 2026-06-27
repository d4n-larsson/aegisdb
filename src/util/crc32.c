/* CRC32 (IEEE 802.3, reflected, poly 0xEDB88820) — T009. */
#include "aegisdb/crc32.h"

static uint32_t g_table[256];
static int g_table_ready = 0;

static void build_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_table[i] = c;
    }
    g_table_ready = 1;
}

uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
    if (!g_table_ready) build_table();
    const unsigned char *p = (const unsigned char *)data;
    crc = ~crc;
    while (len--)
        crc = g_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

uint32_t crc32_compute(const void *data, size_t len) {
    return crc32_update(0, data, len);
}