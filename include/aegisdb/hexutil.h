/* Lowercase hex encoding of a byte buffer. */
#ifndef AEGISDB_HEXUTIL_H
#define AEGISDB_HEXUTIL_H

#include <stddef.h>
#include <stdint.h>

/* Write `n` bytes of `in` as 2*n lowercase hex chars into `out`, NUL-terminated.
 * `out` must have room for 2*n + 1 bytes. */
void aegis_hex_encode(const uint8_t *in, size_t n, char *out);

#endif /* AEGISDB_HEXUTIL_H */