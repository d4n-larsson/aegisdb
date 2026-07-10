/* XChaCha20-Poly1305 AEAD. Self-contained, no deps (see aead.h).
 *
 * Layering:
 *   - ChaCha20 block function + keystream XOR      (RFC 8439 §2.3-2.4)
 *   - HChaCha20 subkey derivation                  (draft-irtf-cfrg-xchacha §2.2)
 *   - Poly1305 one-shot MAC (donna 32-bit core)    (RFC 8439 §2.5)
 *   - XChaCha20-Poly1305 AEAD framing              (RFC 8439 §2.8, extended)
 *
 * The three primitives (aegis_chacha20_block / aegis_hchacha20 / aegis_poly1305)
 * are given external linkage — not for general use (the public API is
 * aead_seal/aead_open in aead.h), but so the unit test can check each against
 * its published known-answer vector, not only the end-to-end AEAD vector. */
#include "aegisdb/aead.h"

#include <string.h>

/* ---------------------------------------------------------- little-endian --- */
static uint32_t u8to32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void u32to8le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void u64to8le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* ----------------------------------------------------------------- ChaCha20 - */
#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d)              \
    a += b; d ^= a; d = ROTL32(d, 16); \
    c += d; b ^= c; b = ROTL32(b, 12); \
    a += b; d ^= a; d = ROTL32(d, 8);  \
    c += d; b ^= c; b = ROTL32(b, 7)

/* 20 rounds (10 column+diagonal double-rounds) in place. */
static void chacha20_rounds(uint32_t x[16]) {
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
}

static void chacha20_state(uint32_t s[16], const uint8_t key[32],
                           uint32_t counter, const uint8_t nonce[12]) {
    s[0] = 0x61707865; /* "expand 32-byte k" */
    s[1] = 0x3320646e;
    s[2] = 0x79622d32;
    s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = u8to32le(key + 4 * i);
    s[12] = counter;
    s[13] = u8to32le(nonce + 0);
    s[14] = u8to32le(nonce + 4);
    s[15] = u8to32le(nonce + 8);
}

/* One 64-byte keystream block for (key, counter, 96-bit nonce). */
void aegis_chacha20_block(const uint8_t key[32], uint32_t counter,
                          const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16], x[16];
    chacha20_state(s, key, counter, nonce);
    memcpy(x, s, sizeof x);
    chacha20_rounds(x);
    for (int i = 0; i < 16; i++) u32to8le(out + 4 * i, x[i] + s[i]);
}

/* XOR `len` bytes of `in` with the keystream starting at `counter`. in==out ok. */
static void chacha20_xor(const uint8_t key[32], uint32_t counter,
                         const uint8_t nonce[12], const uint8_t *in,
                         uint8_t *out, size_t len) {
    uint8_t block[64];
    size_t off = 0;
    while (off < len) {
        aegis_chacha20_block(key, counter, nonce, block);
        size_t n = len - off < 64 ? len - off : 64;
        for (size_t i = 0; i < n; i++) out[off + i] = in[off + i] ^ block[i];
        off += n;
        counter++;
    }
}

/* HChaCha20: derive a 32-byte subkey from the key and the first 16 nonce bytes
 * (the rounds with NO final state addition; output words 0-3 and 12-15). */
void aegis_hchacha20(const uint8_t key[32], const uint8_t nonce16[16],
                     uint8_t out[32]) {
    uint32_t x[16];
    x[0] = 0x61707865;
    x[1] = 0x3320646e;
    x[2] = 0x79622d32;
    x[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) x[4 + i] = u8to32le(key + 4 * i);
    for (int i = 0; i < 4; i++) x[12 + i] = u8to32le(nonce16 + 4 * i);
    chacha20_rounds(x);
    for (int i = 0; i < 4; i++) u32to8le(out + 4 * i, x[i]);
    for (int i = 0; i < 4; i++) u32to8le(out + 16 + 4 * i, x[12 + i]);
}

/* ------------------------------------------------------------------ Poly1305 - */
/* donna 32-bit reference (public-domain, Andrew Moon), incremental so the MAC
 * can stream over an arbitrarily large payload without buffering it. */
typedef struct {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    size_t leftover;
    uint8_t buffer[16];
    uint8_t final;
} poly1305_ctx;

static void poly1305_init(poly1305_ctx *st, const uint8_t key[32]) {
    st->r[0] = (u8to32le(&key[0])) & 0x3ffffff;
    st->r[1] = (u8to32le(&key[3]) >> 2) & 0x3ffff03;
    st->r[2] = (u8to32le(&key[6]) >> 4) & 0x3ffc0ff;
    st->r[3] = (u8to32le(&key[9]) >> 6) & 0x3f03fff;
    st->r[4] = (u8to32le(&key[12]) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) st->h[i] = 0;
    st->pad[0] = u8to32le(&key[16]);
    st->pad[1] = u8to32le(&key[20]);
    st->pad[2] = u8to32le(&key[24]);
    st->pad[3] = u8to32le(&key[28]);
    st->leftover = 0;
    st->final = 0;
}

static void poly1305_blocks(poly1305_ctx *st, const uint8_t *m, size_t bytes) {
    const uint32_t hibit = st->final ? 0 : (1u << 24);
    uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3],
             r4 = st->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3],
             h4 = st->h[4];

    while (bytes >= 16) {
        uint64_t d0, d1, d2, d3, d4;
        uint32_t c;
        /* h += m */
        h0 += (u8to32le(m + 0)) & 0x3ffffff;
        h1 += (u8to32le(m + 3) >> 2) & 0x3ffffff;
        h2 += (u8to32le(m + 6) >> 4) & 0x3ffffff;
        h3 += (u8to32le(m + 9) >> 6) & 0x3ffffff;
        h4 += (u8to32le(m + 12) >> 8) | hibit;
        /* h *= r */
        d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 +
             (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 +
             (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 +
             (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 +
             (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 +
             (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        /* (partial) h %= p */
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = (h0 >> 26); h0 &= 0x3ffffff;
        h1 += c;
        m += 16;
        bytes -= 16;
    }
    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h[4] = h4;
}

static void poly1305_update(poly1305_ctx *st, const uint8_t *m, size_t bytes) {
    if (st->leftover) {
        size_t want = 16 - st->leftover;
        if (want > bytes) want = bytes;
        memcpy(st->buffer + st->leftover, m, want);
        bytes -= want;
        m += want;
        st->leftover += want;
        if (st->leftover < 16) return;
        poly1305_blocks(st, st->buffer, 16);
        st->leftover = 0;
    }
    if (bytes >= 16) {
        size_t want = bytes & ~(size_t)15;
        poly1305_blocks(st, m, want);
        m += want;
        bytes -= want;
    }
    if (bytes) {
        memcpy(st->buffer + st->leftover, m, bytes);
        st->leftover += bytes;
    }
}

static void poly1305_finish(poly1305_ctx *st, uint8_t mac[16]) {
    uint32_t h0, h1, h2, h3, h4, c;
    uint32_t g0, g1, g2, g3, g4;
    uint64_t f;
    uint32_t mask;

    if (st->leftover) {
        size_t i = st->leftover;
        st->buffer[i++] = 1;
        for (; i < 16; i++) st->buffer[i] = 0;
        st->final = 1;
        poly1305_blocks(st, st->buffer, 16);
    }

    h0 = st->h[0]; h1 = st->h[1]; h2 = st->h[2]; h3 = st->h[3]; h4 = st->h[4];
    c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    g4 = h4 + c - (1u << 26);

    /* select h if h < p, else h + -p (constant-time) */
    mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    /* serialize to 128 bits */
    h0 = (h0) | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);

    f = (uint64_t)h0 + st->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + st->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + st->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + st->pad[3] + (f >> 32); h3 = (uint32_t)f;

    u32to8le(mac + 0, h0);
    u32to8le(mac + 4, h1);
    u32to8le(mac + 8, h2);
    u32to8le(mac + 12, h3);
}

void aegis_poly1305(uint8_t mac[16], const uint8_t *m, size_t len,
                    const uint8_t key[32]) {
    poly1305_ctx st;
    poly1305_init(&st, key);
    poly1305_update(&st, m, len);
    poly1305_finish(&st, mac);
}

/* ---------------------------------------------------- XChaCha20-Poly1305 AEAD */
/* Feed zero padding to align the MAC input to a 16-byte boundary (RFC 8439). */
static void poly_pad16(poly1305_ctx *st, size_t len) {
    static const uint8_t zero[16] = {0};
    size_t rem = len & 15u;
    if (rem) poly1305_update(st, zero, 16 - rem);
}

/* Derive the XChaCha20 subkey + 96-bit ChaCha20 nonce from a 192-bit nonce, and
 * compute the Poly1305 one-time key (block 0). Shared by seal and open. */
static void xchacha_setup(const uint8_t key[AEAD_KEY_LEN],
                          const uint8_t nonce[AEAD_NONCE_LEN], uint8_t subkey[32],
                          uint8_t cc_nonce[12], uint8_t polykey[32]) {
    aegis_hchacha20(key, nonce, subkey);
    memset(cc_nonce, 0, 4);
    memcpy(cc_nonce + 4, nonce + 16, 8);
    uint8_t block0[64];
    aegis_chacha20_block(subkey, 0, cc_nonce, block0);
    memcpy(polykey, block0, 32); /* first 32 bytes are the Poly1305 key */
}

/* MAC over aad || pad16 || ct || pad16 || le64(aad_len) || le64(ct_len). */
static void aead_mac(const uint8_t polykey[32], const uint8_t *aad,
                     size_t aad_len, const uint8_t *ct, size_t ct_len,
                     uint8_t tag[AEAD_TAG_LEN]) {
    poly1305_ctx st;
    poly1305_init(&st, polykey);
    if (aad_len) poly1305_update(&st, aad, aad_len);
    poly_pad16(&st, aad_len);
    if (ct_len) poly1305_update(&st, ct, ct_len);
    poly_pad16(&st, ct_len);
    uint8_t lens[16];
    u64to8le(lens + 0, aad_len);
    u64to8le(lens + 8, ct_len);
    poly1305_update(&st, lens, 16);
    poly1305_finish(&st, tag);
}

void aead_seal(const uint8_t key[AEAD_KEY_LEN],
               const uint8_t nonce[AEAD_NONCE_LEN], const uint8_t *aad,
               size_t aad_len, const uint8_t *pt, size_t pt_len, uint8_t *ct,
               uint8_t tag[AEAD_TAG_LEN]) {
    uint8_t subkey[32], cc_nonce[12], polykey[32];
    xchacha_setup(key, nonce, subkey, cc_nonce, polykey);
    /* data starts at counter 1 (counter 0 produced the Poly1305 key) */
    chacha20_xor(subkey, 1, cc_nonce, pt, ct, pt_len);
    aead_mac(polykey, aad, aad_len, ct, pt_len, tag);
}

/* Constant-time 16-byte compare. */
static int ct_eq16(const uint8_t *a, const uint8_t *b) {
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

int aead_open(const uint8_t key[AEAD_KEY_LEN],
              const uint8_t nonce[AEAD_NONCE_LEN], const uint8_t *aad,
              size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *pt,
              const uint8_t tag[AEAD_TAG_LEN]) {
    uint8_t subkey[32], cc_nonce[12], polykey[32], expect[AEAD_TAG_LEN];
    xchacha_setup(key, nonce, subkey, cc_nonce, polykey);
    /* Verify before decrypting: never expose unauthenticated plaintext. */
    aead_mac(polykey, aad, aad_len, ct, ct_len, expect);
    if (!ct_eq16(expect, tag)) {
        memset(pt, 0, ct_len);
        return -1;
    }
    chacha20_xor(subkey, 1, cc_nonce, ct, pt, ct_len);
    return 0;
}