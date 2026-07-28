/* Little-endian integer serialization primitives.
 *
 * All of AegisDB's on-disk and wire encodings are little-endian; these are the
 * single source of truth for packing/unpacking scalars into byte buffers. They
 * are explicit shift/or (not a memcpy of the host representation), so the byte
 * order is fixed regardless of the host's endianness. */
#ifndef AEGISDB_ENDIAN_H
#define AEGISDB_ENDIAN_H

#include <stdint.h>

static inline void aegis_put_u16le(uint8_t *b, uint16_t v) {
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

static inline void aegis_put_u32le(uint8_t *b, uint32_t v) {
    for (int i = 0; i < 4; i++)
        b[i] = (uint8_t)(v >> (8 * i));
}

static inline void aegis_put_u64le(uint8_t *b, uint64_t v) {
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t)(v >> (8 * i));
}

static inline uint16_t aegis_get_u16le(const uint8_t *b) {
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static inline uint32_t aegis_get_u32le(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static inline uint64_t aegis_get_u64le(const uint8_t *b) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)b[i] << (8 * i);
    return v;
}

#endif /* AEGISDB_ENDIAN_H */