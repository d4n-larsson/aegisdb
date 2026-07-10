/* Encryption envelope for index checkpoints (encryption at rest, see
 * docs/encryption-at-rest-design.md).
 *
 * Checkpoints (memory.index, memory.sem) are serialized into a single buffer by
 * their owners; these helpers are the file boundary. With no key they read/write
 * that buffer verbatim (the historical plaintext format). With a key the buffer
 * is wrapped as whole-file AEAD:
 *   [MAGIC u32][VERSION u32][NONCE 24B][CIPHERTEXT: buf][TAG 16B]
 * A wrong key or a tampered file makes ckpt_read fail, and every caller treats a
 * failed checkpoint load as "missing" — recovery then rebuilds from the log. */
#ifndef AEGISDB_CKPT_CRYPT_H
#define AEGISDB_CKPT_CRYPT_H

#include <stddef.h>
#include <stdint.h>

/* Atomically write `plain`/`plain_len` to `path` (tmp + fsync + rename),
 * encrypting under `key` (AEAD_KEY_LEN bytes) when non-NULL. Returns 0/-1. */
int ckpt_write(const char *path, const uint8_t *key, const uint8_t *plain,
               size_t plain_len);

/* Read `path` into a freshly malloc'd plaintext buffer (out/out_len; free with
 * free()), decrypting under `key` when non-NULL. Returns 0 on success; -1 on I/O
 * error, format/version mismatch, wrong key, or tamper. */
int ckpt_read(const char *path, const uint8_t *key, uint8_t **out,
              size_t *out_len);

#endif /* AEGISDB_CKPT_CRYPT_H */