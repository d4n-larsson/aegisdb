/* MemoryRecord lifecycle and binary log codec (T010, extended T036/T046). */
#include "aegisdb/record.h"

#include <stdlib.h>
#include <string.h>

/* v1: single embedding (dim + dim floats). v2 (#85): vec_count + dim +
 * vec_count*dim floats. decode reads both; encode always writes v2. */
#define RECORD_CODEC_VERSION 2
#define NULL_LEN 0xFFFFFFFFu

/* ----- record lifecycle ------------------------------------------------- */

void record_init(MemoryRecord *r) {
    memset(r, 0, sizeof(*r));
    r->importance = 0.0f;
    r->confidence = 1.0f;
}

void record_free(MemoryRecord *r) {
    if (!r) return;
    free(r->agent_id);
    for (size_t i = 0; i < r->tag_count; i++) free(r->tags[i]);
    free(r->tags);
    free(r->embedding);
    for (size_t i = 0; i < r->rel_count; i++) free(r->relationships[i].kind);
    free(r->relationships);
    free(r->data);
    memset(r, 0, sizeof(*r));
}

static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

int record_set_tags(MemoryRecord *r, const char *const *tags, size_t n) {
    for (size_t i = 0; i < r->tag_count; i++) free(r->tags[i]);
    free(r->tags);
    r->tags = NULL;
    r->tag_count = 0;
    if (n == 0) return 0;
    r->tags = calloc(n, sizeof(char *));
    if (!r->tags) return -1;
    for (size_t i = 0; i < n; i++) {
        r->tags[i] = dup_str(tags[i]);
        if (!r->tags[i]) {
            for (size_t j = 0; j < i; j++) free(r->tags[j]);
            free(r->tags);
            r->tags = NULL;
            return -1;
        }
    }
    r->tag_count = n;
    return 0;
}

int record_add_relationship(MemoryRecord *r, uint64_t from_id, uint64_t to_id,
                            const char *kind) {
    Relationship *na =
        realloc(r->relationships, (r->rel_count + 1) * sizeof(Relationship));
    if (!na) return -1;
    r->relationships = na;
    Relationship *e = &r->relationships[r->rel_count];
    e->from_id = from_id;
    e->to_id = to_id;
    e->kind = kind ? dup_str(kind) : NULL;
    if (kind && !e->kind) return -1;
    r->rel_count++;
    return 0;
}

MemoryRecord *record_clone(const MemoryRecord *src) {
    MemoryRecord *r = malloc(sizeof(*r));
    if (!r) return NULL;
    record_init(r);
    r->id = src->id;
    r->type = src->type;
    r->created = src->created;
    r->updated = src->updated;
    r->importance = src->importance;
    r->confidence = src->confidence;
    r->expires_at = src->expires_at;
    r->deleted = src->deleted;

    if (src->agent_id) {
        r->agent_id = dup_str(src->agent_id);
        if (!r->agent_id) goto fail;
    }
    if (src->tag_count &&
        record_set_tags(r, (const char *const *)src->tags, src->tag_count))
        goto fail;
    if (src->embedding_dim && src->vec_count) {
        size_t n = src->vec_count * src->embedding_dim;
        r->embedding = malloc(n * sizeof(float));
        if (!r->embedding) goto fail;
        memcpy(r->embedding, src->embedding, n * sizeof(float));
        r->embedding_dim = src->embedding_dim;
        r->vec_count = src->vec_count;
    }
    for (size_t i = 0; i < src->rel_count; i++) {
        if (record_add_relationship(r, src->relationships[i].from_id,
                                    src->relationships[i].to_id,
                                    src->relationships[i].kind))
            goto fail;
    }
    if (src->data_len) {
        r->data = malloc(src->data_len);
        if (!r->data) goto fail;
        memcpy(r->data, src->data, src->data_len);
        r->data_len = src->data_len;
    }
    return r;
fail:
    record_free(r);
    free(r);
    return NULL;
}

/* ----- little-endian serialization buffer ------------------------------- */

typedef struct {
    uint8_t *p;
    size_t len;
    size_t cap;
    int err;
} Buf;

static void buf_reserve(Buf *b, size_t extra) {
    if (b->err) return;
    if (b->len + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap * 2 : 128;
    while (cap < b->len + extra) cap *= 2;
    uint8_t *np = realloc(b->p, cap);
    if (!np) { b->err = 1; return; }
    b->p = np;
    b->cap = cap;
}

static void put_bytes(Buf *b, const void *s, size_t n) {
    buf_reserve(b, n);
    if (b->err) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
}
static void put_u8(Buf *b, uint8_t v) { put_bytes(b, &v, 1); }
static void put_u16(Buf *b, uint16_t v) {
    uint8_t t[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    put_bytes(b, t, 2);
}
static void put_u32(Buf *b, uint32_t v) {
    uint8_t t[4];
    for (int i = 0; i < 4; i++) t[i] = (uint8_t)(v >> (8 * i));
    put_bytes(b, t, 4);
}
static void put_u64(Buf *b, uint64_t v) {
    uint8_t t[8];
    for (int i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8 * i));
    put_bytes(b, t, 8);
}
static void put_f32(Buf *b, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    put_u32(b, u);
}
static void put_lenstr(Buf *b, const char *s, size_t n) {
    if (!s) { put_u32(b, NULL_LEN); return; }
    put_u32(b, (uint32_t)n);
    put_bytes(b, s, n);
}

int record_encode(const MemoryRecord *r, uint8_t **out, size_t *out_len) {
    Buf b = {0};
    put_u8(&b, RECORD_CODEC_VERSION);
    put_u64(&b, r->id);
    put_u8(&b, (uint8_t)r->type);
    put_u64(&b, r->created);
    put_u64(&b, r->updated);
    put_f32(&b, r->importance);
    put_f32(&b, r->confidence);
    put_u8(&b, (uint8_t)(r->deleted ? 1 : 0));
    put_u64(&b, r->expires_at);
    put_lenstr(&b, r->agent_id, r->agent_id ? strlen(r->agent_id) : 0);

    put_u16(&b, (uint16_t)r->tag_count);
    for (size_t i = 0; i < r->tag_count; i++)
        put_lenstr(&b, r->tags[i], strlen(r->tags[i]));

    /* v2: vec_count, dim, then vec_count*dim floats (contiguous vectors). */
    put_u32(&b, (uint32_t)r->vec_count);
    put_u32(&b, (uint32_t)r->embedding_dim);
    for (size_t i = 0; i < r->vec_count * r->embedding_dim; i++)
        put_f32(&b, r->embedding[i]);

    /* The wire count is u16; truncation here would produce an undecodable frame
     * (durable data loss). qe_relate caps rel_count far below this, so tripping
     * it means a caller built a pathological record — refuse to encode it. */
    if (r->rel_count > UINT16_MAX) { free(b.p); return -1; }
    put_u16(&b, (uint16_t)r->rel_count);
    for (size_t i = 0; i < r->rel_count; i++) {
        put_u64(&b, r->relationships[i].from_id);
        put_u64(&b, r->relationships[i].to_id);
        put_lenstr(&b, r->relationships[i].kind,
                   r->relationships[i].kind ? strlen(r->relationships[i].kind)
                                            : 0);
    }

    put_u32(&b, (uint32_t)r->data_len);
    put_bytes(&b, r->data, r->data_len);

    if (b.err) { free(b.p); return -1; }
    *out = b.p;
    *out_len = b.len;
    return 0;
}

/* ----- decode cursor ---------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t off;
    int err;
} Cur;

static int cur_take(Cur *c, void *dst, size_t n) {
    if (c->err || c->off + n > c->len) { c->err = 1; return -1; }
    if (dst) memcpy(dst, c->p + c->off, n);
    c->off += n;
    return 0;
}
static uint8_t get_u8(Cur *c) { uint8_t v = 0; cur_take(c, &v, 1); return v; }
static uint16_t get_u16(Cur *c) {
    uint8_t t[2] = {0};
    cur_take(c, t, 2);
    return (uint16_t)(t[0] | (t[1] << 8));
}
static uint32_t get_u32(Cur *c) {
    uint8_t t[4] = {0};
    cur_take(c, t, 4);
    return (uint32_t)t[0] | ((uint32_t)t[1] << 8) | ((uint32_t)t[2] << 16) |
           ((uint32_t)t[3] << 24);
}
static uint64_t get_u64(Cur *c) {
    uint8_t t[8] = {0};
    cur_take(c, t, 8);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)t[i] << (8 * i);
    return v;
}
static float get_f32(Cur *c) {
    uint32_t u = get_u32(c);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
/* Returns malloc'd NUL-terminated string, or NULL for the null marker. On
 * allocation failure or truncation sets c->err and returns NULL. */
static char *get_lenstr(Cur *c, int *was_null) {
    uint32_t n = get_u32(c);
    if (was_null) *was_null = 0;
    if (n == NULL_LEN) { if (was_null) *was_null = 1; return NULL; }
    /* subtraction-form (off <= len invariant): cannot overflow, unlike off + n */
    if (c->err || n > c->len - c->off) { c->err = 1; return NULL; }
    char *s = malloc((size_t)n + 1);
    if (!s) { c->err = 1; return NULL; }
    memcpy(s, c->p + c->off, n);
    s[n] = '\0';
    c->off += n;
    return s;
}

int record_decode(const uint8_t *buf, size_t len, MemoryRecord *out) {
    Cur c = {buf, len, 0, 0};
    record_init(out);

    uint8_t ver = get_u8(&c);
    if (ver != 1 && ver != 2) goto fail; /* read v1 (single vec) and v2 */
    out->id = get_u64(&c);
    out->type = (MemoryType)get_u8(&c);
    out->created = get_u64(&c);
    out->updated = get_u64(&c);
    out->importance = get_f32(&c);
    out->confidence = get_f32(&c);
    out->deleted = get_u8(&c);
    out->expires_at = get_u64(&c);

    int wasnull;
    out->agent_id = get_lenstr(&c, &wasnull);
    if (c.err) goto fail;

    uint16_t tc = get_u16(&c);
    if (c.err) goto fail;
    if (tc) {
        out->tags = calloc(tc, sizeof(char *));
        if (!out->tags) goto fail;
        for (uint16_t i = 0; i < tc; i++) {
            out->tags[i] = get_lenstr(&c, NULL);
            if (c.err || !out->tags[i]) goto fail;
            out->tag_count = (size_t)i + 1;
        }
    }

    /* v1: [dim][dim floats] (implicitly one vector). v2: [vec_count][dim]
     * [vec_count*dim floats]. */
    uint32_t vec_count = (ver >= 2) ? get_u32(&c) : 1;
    uint32_t dim = get_u32(&c);
    if (c.err) goto fail;
    if (ver == 1 && dim == 0) vec_count = 0; /* v1 with no embedding */
    if (dim && vec_count) {
        /* `vec_count`/`dim` are attacker-controlled on the decode path (a
         * replicated frame, or a tampered/corrupt log). Bound the float count
         * against the payload BEFORE any multiply: computing total*4 first would
         * overflow size_t (e.g. vec_count=dim=2^31 -> total*4 wraps to 0),
         * defeating the check and undersizing the malloc -> heap overflow.
         * Division-form checks cannot overflow (c.off <= c.len invariant). */
        size_t avail_floats = (c.len - c.off) / 4;
        if (vec_count > avail_floats / dim) goto fail; /* total > payload */
        size_t total = (size_t)vec_count * dim;        /* <= avail_floats now */
        out->embedding = malloc(total * sizeof(float));
        if (!out->embedding) goto fail;
        for (size_t i = 0; i < total; i++) {
            out->embedding[i] = get_f32(&c);
            if (c.err) goto fail; /* every read is in-bounds; guard anyway */
        }
        out->embedding_dim = dim;
        out->vec_count = vec_count;
    }

    uint16_t rc = get_u16(&c);
    if (c.err) goto fail;
    for (uint16_t i = 0; i < rc; i++) {
        uint64_t from = get_u64(&c);
        uint64_t to = get_u64(&c);
        char *kind = get_lenstr(&c, &wasnull);
        if (c.err) { free(kind); goto fail; }
        int rv = record_add_relationship(out, from, to, kind);
        free(kind);
        if (rv) goto fail;
    }

    uint32_t dl = get_u32(&c);
    if (c.err || dl > c.len - c.off) goto fail; /* subtraction-form: no overflow */
    if (dl) {
        out->data = malloc(dl);
        if (!out->data) goto fail;
        memcpy(out->data, c.p + c.off, dl);
        c.off += dl;
    }
    out->data_len = dl;

    if (c.err) goto fail;
    return 0;
fail:
    record_free(out);
    return -1;
}