/* SHA-256 (FIPS 180-4), one-shot. Used to hash bearer tokens at rest so a
 * leaked token file does not reveal usable secrets. Tokens are expected to be
 * high-entropy random strings, for which an unsalted cryptographic hash is
 * sufficient. */
#ifndef AEGISDB_SHA256_H
#define AEGISDB_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32

/* Compute the SHA-256 digest of `data` into `out` (32 bytes). */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

#endif /* AEGISDB_SHA256_H */