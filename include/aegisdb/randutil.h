/* Cryptographic randomness. */
#ifndef AEGISDB_RANDUTIL_H
#define AEGISDB_RANDUTIL_H

#include <stddef.h>
#include <stdint.h>

/* Fill `n` bytes of `p` with cryptographic randomness (e.g. frame/envelope
 * nonces). Returns 0 on success, -1 on error. */
int aegis_fill_random(uint8_t *p, size_t n);

#endif /* AEGISDB_RANDUTIL_H */