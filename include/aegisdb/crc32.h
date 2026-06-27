/* CRC32 (IEEE 802.3) checksum utility (T009). */
#ifndef AEGISDB_CRC32_H
#define AEGISDB_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* Compute the CRC32 of a buffer. */
uint32_t crc32_compute(const void *data, size_t len);

/* Incremental CRC32: pass 0 as the initial crc. */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

#endif /* AEGISDB_CRC32_H */