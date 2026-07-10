/* Checkpoint encryption envelope (see ckpt_crypt.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aegisdb/ckpt_crypt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include "aegisdb/aead.h"

#define CKPT_MAGIC 0x43454B41u /* "AKEC" LE: AegisDB checKpoint EnCryption */
#define CKPT_VERSION 1u
#define CKPT_HDR 32 /* magic(4) + version(4) + nonce(24); also the AEAD AAD */

static void put_u32le(uint8_t *b, uint32_t v) {
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get_u32le(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static int fill_random(uint8_t *p, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(p + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

/* Write `buf`/`len` to `path` atomically: tmp + fsync + rename. */
static int write_atomic(const char *path, const uint8_t *buf, size_t len) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int ok = 1;
    for (size_t off = 0; off < len;) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            ok = 0;
            break;
        }
        off += (size_t)w;
    }
    if (ok) ok = (fsync(fd) == 0);
    close(fd);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Read the whole file at `path` into a fresh buffer. Returns 0/-1. */
static int read_whole(const char *path, uint8_t **out, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0 || lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return -1;
    }
    uint8_t *buf = malloc((size_t)size ? (size_t)size : 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    size_t got = 0;
    while (got < (size_t)size) {
        ssize_t r = read(fd, buf + got, (size_t)size - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            close(fd);
            free(buf);
            return -1;
        }
        got += (size_t)r;
    }
    close(fd);
    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

int ckpt_write(const char *path, const uint8_t *key, const uint8_t *plain,
               size_t plain_len) {
    if (!key) return write_atomic(path, plain, plain_len);

    size_t total = CKPT_HDR + plain_len + AEAD_TAG_LEN;
    uint8_t *env = malloc(total);
    if (!env) return -1;
    put_u32le(env, CKPT_MAGIC);
    put_u32le(env + 4, CKPT_VERSION);
    if (fill_random(env + 8, AEAD_NONCE_LEN) != 0) {
        free(env);
        return -1;
    }
    aead_seal(key, env + 8, env, CKPT_HDR, plain, plain_len, env + CKPT_HDR,
              env + CKPT_HDR + plain_len);
    int rv = write_atomic(path, env, total);
    free(env);
    return rv;
}

int ckpt_read(const char *path, const uint8_t *key, uint8_t **out,
              size_t *out_len) {
    uint8_t *file = NULL;
    size_t flen = 0;
    if (read_whole(path, &file, &flen) != 0) return -1;

    if (!key) {
        *out = file;
        *out_len = flen;
        return 0;
    }

    if (flen < CKPT_HDR + AEAD_TAG_LEN || get_u32le(file) != CKPT_MAGIC ||
        get_u32le(file + 4) != CKPT_VERSION) {
        free(file);
        return -1;
    }
    size_t plain_len = flen - CKPT_HDR - AEAD_TAG_LEN;
    uint8_t *plain = malloc(plain_len ? plain_len : 1);
    if (!plain) {
        free(file);
        return -1;
    }
    /* AAD is the header (magic+version+nonce); tag trails the ciphertext. */
    if (aead_open(key, file + 8, file, CKPT_HDR, file + CKPT_HDR, plain_len,
                  plain, file + CKPT_HDR + plain_len) != 0) {
        free(plain);
        free(file);
        return -1;
    }
    free(file);
    *out = plain;
    *out_len = plain_len;
    return 0;
}