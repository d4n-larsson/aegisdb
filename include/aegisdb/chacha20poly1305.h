/* Low-level XChaCha20-Poly1305 primitives.
 *
 * The public AEAD interface is aead.h (aead_seal/aead_open). These building
 * blocks are declared here ONLY so the known-answer tests in
 * tests/unit/test_aead.c can validate each against its published RFC 8439 /
 * draft-irtf-cfrg-xchacha vector — not for general use. chacha20poly1305.c
 * includes this header so the definitions are checked against these
 * declarations, and the test includes it instead of hand-copying prototypes
 * (which could silently drift from the implementation). */
#ifndef AEGISDB_CHACHA20POLY1305_H
#define AEGISDB_CHACHA20POLY1305_H

#include <stddef.h>
#include <stdint.h>

void aegis_chacha20_block(const uint8_t key[32], uint32_t counter,
                          const uint8_t nonce[12], uint8_t out[64]);
void aegis_hchacha20(const uint8_t key[32], const uint8_t nonce16[16],
                     uint8_t out[32]);
void aegis_poly1305(uint8_t mac[16], const uint8_t *m, size_t len,
                    const uint8_t key[32]);

#endif /* AEGISDB_CHACHA20POLY1305_H */