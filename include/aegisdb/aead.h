/* XChaCha20-Poly1305 AEAD (authenticated encryption with associated data).
 *
 * Self-contained, no external dependency — vendored the way sha256.c is, to keep
 * AegisDB a single dependency-free binary. This is the cipher for encryption at
 * rest (see docs/encryption-at-rest-design.md): log frame payloads and index
 * checkpoints are sealed with it.
 *
 * XChaCha20 (ChaCha20 with an HChaCha20-derived subkey) gives a 192-bit nonce,
 * so a per-message nonce can be drawn at RANDOM with negligible collision
 * probability — no nonce counter to persist and no (key, nonce) reuse hazard
 * across compaction or replica promotion.
 *
 * Construction: RFC 8439 ChaCha20 + Poly1305, extended to XChaCha20 per
 * draft-irtf-cfrg-xchacha. Validated against the published test vectors in
 * tests/unit/test_aead.c. */
#ifndef AEGISDB_AEAD_H
#define AEGISDB_AEAD_H

#include <stddef.h>
#include <stdint.h>

#define AEAD_KEY_LEN 32   /* 256-bit key */
#define AEAD_NONCE_LEN 24 /* 192-bit XChaCha20 nonce (safe to choose at random) */
#define AEAD_TAG_LEN 16   /* Poly1305 authentication tag */

/* Encrypt `pt` (pt_len bytes) under `key`/`nonce`, authenticating both the
 * ciphertext and `aad` (aad_len bytes; may be NULL/0). Writes pt_len ciphertext
 * bytes to `ct` and the AEAD_TAG_LEN tag to `tag`. `ct` may alias `pt` (in-place
 * encryption). The same (key, nonce) pair must never seal two different
 * messages — draw a fresh random nonce per message. */
void aead_seal(const uint8_t key[AEAD_KEY_LEN],
               const uint8_t nonce[AEAD_NONCE_LEN], const uint8_t *aad,
               size_t aad_len, const uint8_t *pt, size_t pt_len, uint8_t *ct,
               uint8_t tag[AEAD_TAG_LEN]);

/* Verify `tag` over `aad` + `ct` (ct_len bytes) and, only if it is valid,
 * decrypt into `pt` (ct_len bytes; may alias `ct`). Returns 0 on success. On
 * authentication failure returns -1 and leaves `pt` zeroed (never a partial or
 * unauthenticated plaintext). The tag comparison is constant-time. */
int aead_open(const uint8_t key[AEAD_KEY_LEN],
              const uint8_t nonce[AEAD_NONCE_LEN], const uint8_t *aad,
              size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *pt,
              const uint8_t tag[AEAD_TAG_LEN]);

#endif /* AEGISDB_AEAD_H */