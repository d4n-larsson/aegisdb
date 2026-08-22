/* MemoryRecord lifecycle and binary log codec (T010, extended T036/T046). */
#include "aegisdb/record.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "aegisdb/endian.h"

/* v1: single embedding (dim + dim floats). v2 (#85): vec_count + dim +
 * vec_count*dim floats. v3 (ROADMAP 5.2): an optional typed fact between the
 * relationships and the payload. v4 (ROADMAP 5.3): an optional derivation
 * between the fact and the payload.
 *
 * decode reads all three. encode writes the *lowest* version that can represent
 * the record: v4 only when a derivation is present, v3 when a fact is but a
 * derivation is not, v2 otherwise. That is the whole
 * compatibility story — an existing log stays readable, and a deployment that
 * never writes a fact keeps producing byte-identical frames, so nothing about
 * its on-disk or replication behaviour changes until it opts in.
 *
 * A tempting alternative was to append the fact *after* the payload and leave
 * the version at 2: decode ignores trailing bytes, so an older reader would
 * accept the frame and simply not see the fact. Rejected — that is silent field
 * loss on whoever is behind, and a replica that quietly drops facts is worse
 * than one that refuses the frame and says so. The refusal is made explicit by
 * the replication handshake gate (see docs/typed-facts-design.md §4).
 *
 * The version constants live in record.h: replication compares them across
 * peers, so they are part of the contract rather than a codec-local detail. */
#define NULL_LEN 0xFFFFFFFFu

/* ----- record lifecycle ------------------------------------------------- */

void record_init(MemoryRecord *r) {
    memset(r, 0, sizeof(*r));
    r->importance = 0.0F;
    r->confidence = 1.0F;
}

void record_free(MemoryRecord *r) {
    if (!r) {
        return;
    }
    free(r->agent_id);
    for (size_t i = 0; i < r->tag_count; i++) {
        free(r->tags[i]);
    }
    free(r->tags);
    free(r->embedding);
    for (size_t i = 0; i < r->rel_count; i++) {
        free(r->relationships[i].kind);
    }
    free(r->relationships);
    free(r->fact.predicate);
    free(r->fact.object_str);
    free(r->derivation.premises);
    free(r->data);
    memset(r, 0, sizeof(*r));
}

static char *dup_str(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

int record_set_tags(MemoryRecord *r, const char *const *tags, size_t n) {
    for (size_t i = 0; i < r->tag_count; i++) {
        free(r->tags[i]);
    }
    free(r->tags);
    r->tags = NULL;
    r->tag_count = 0;
    if (n == 0) {
        return 0;
    }
    r->tags = calloc(n, sizeof(char *));
    if (!r->tags) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        r->tags[i] = dup_str(tags[i]);
        if (!r->tags[i]) {
            for (size_t j = 0; j < i; j++) {
                free(r->tags[j]);
            }
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
    if (!na) {
        return -1;
    }
    r->relationships = na;
    Relationship *e = &r->relationships[r->rel_count];
    e->from_id = from_id;
    e->to_id = to_id;
    e->kind = kind ? dup_str(kind) : NULL;
    if (kind && !e->kind) {
        return -1;
    }
    r->rel_count++;
    return 0;
}

MemoryRecord *record_clone(const MemoryRecord *src) {
    MemoryRecord *r = malloc(sizeof(*r));
    if (!r) {
        return NULL;
    }
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
        if (!r->agent_id) {
            goto fail;
        }
    }
    if (src->tag_count &&
        record_set_tags(r, (const char *const *)src->tags, src->tag_count)) {
        goto fail;
    }
    if (src->fact.kind != FACT_NONE &&
        record_set_fact(r, src->fact.kind, src->fact.subject,
                        src->fact.predicate, src->fact.object_id,
                        src->fact.object_str) != 0) {
        goto fail;
    }
    if (src->derivation.rule != DERIV_NONE &&
        record_set_derivation(r, src->derivation.rule, src->derivation.depth,
                              src->derivation.premises,
                              src->derivation.premise_count) != 0) {
        goto fail;
    }
    if (src->embedding_dim && src->vec_count) {
        /* Overflow-safe: bound vec_count*dim and the *sizeof(float) allocation
         * before multiplying (division form, like record_decode). Records reach
         * clone already validated, so this is defense in depth — but it keeps a
         * pathological in-memory record from turning into a heap overflow. */
        if (src->vec_count > (SIZE_MAX / sizeof(float)) / src->embedding_dim) {
            goto fail;
        }
        size_t n = src->vec_count * src->embedding_dim;
        r->embedding = malloc(n * sizeof(float));
        if (!r->embedding) {
            goto fail;
        }
        memcpy(r->embedding, src->embedding, n * sizeof(float));
        r->embedding_dim = src->embedding_dim;
        r->vec_count = src->vec_count;
    }
    for (size_t i = 0; i < src->rel_count; i++) {
        if (record_add_relationship(r, src->relationships[i].from_id,
                                    src->relationships[i].to_id,
                                    src->relationships[i].kind)) {
            goto fail;
        }
    }
    if (src->data_len) {
        r->data = malloc(src->data_len);
        if (!r->data) {
            goto fail;
        }
        memcpy(r->data, src->data, src->data_len);
        r->data_len = src->data_len;
    }
    return r;
fail:
    record_free(r);
    free(r);
    return NULL;
}

int record_set_fact(MemoryRecord *r, FactKind kind, uint64_t subject,
                    const char *predicate, uint64_t object_id,
                    const char *object_str) {
    if (kind == FACT_NONE) {
        free(r->fact.predicate);
        free(r->fact.object_str);
        memset(&r->fact, 0, sizeof(r->fact));
        return 0;
    }
    if (kind != FACT_OBJ_ID && kind != FACT_OBJ_STRING) {
        return -1;
    }
    if (!predicate || !*predicate) {
        return -1;
    }
    if (kind == FACT_OBJ_STRING && !object_str) {
        return -1;
    }
    /* Build the copies before touching the record, so a failure halfway leaves
     * the previous fact intact rather than half-replaced. */
    char *pred = dup_str(predicate);
    char *obj = (kind == FACT_OBJ_STRING) ? dup_str(object_str) : NULL;
    if (!pred || (kind == FACT_OBJ_STRING && !obj)) {
        free(pred);
        free(obj);
        return -1;
    }
    free(r->fact.predicate);
    free(r->fact.object_str);
    r->fact.kind = kind;
    r->fact.subject = subject;
    r->fact.predicate = pred;
    r->fact.object_id = (kind == FACT_OBJ_ID) ? object_id : 0;
    r->fact.object_str = obj;
    return 0;
}

int record_set_derivation(MemoryRecord *r, DerivRule rule, uint16_t depth,
                          const uint64_t *premises, size_t n) {
    if (rule == DERIV_NONE) {
        free(r->derivation.premises);
        memset(&r->derivation, 0, sizeof(r->derivation));
        return 0;
    }
    if (rule != DERIV_TRANSITIVE && rule != DERIV_SYMMETRIC &&
        rule != DERIV_INVERSE) {
        return -1;
    }
    /* A derivation with no premises is not a derivation — it would claim
     * provenance while naming none, which is worse than claiming none. */
    if (!premises || n == 0 || n > DERIV_MAX_PREMISES) {
        return -1;
    }
    /* Copy before touching the record, so a failure halfway leaves the previous
     * derivation intact rather than half-replaced (as record_set_fact does). */
    uint64_t *copy = malloc(n * sizeof(*copy));
    if (!copy) {
        return -1;
    }
    memcpy(copy, premises, n * sizeof(*copy));
    free(r->derivation.premises);
    r->derivation.rule = rule;
    r->derivation.depth = depth;
    r->derivation.premises = copy;
    r->derivation.premise_count = n;
    return 0;
}

/* ----- little-endian serialization buffer ------------------------------- */

typedef struct {
    uint8_t *p;
    size_t len;
    size_t cap;
    int err;
} Buf;

static void buf_reserve(Buf *b, size_t extra) {
    if (b->err) {
        return;
    }
    if (b->len + extra < b->len) {
        b->err = 1;
        return;
    } /* size_t overflow */
    if (b->len + extra <= b->cap) {
        return;
    }
    size_t cap = b->cap ? b->cap * 2 : 128;
    while (cap < b->len + extra) {
        size_t next = cap * 2;
        if (next < cap) {
            b->err = 1;
            return;
        } /* doubling overflowed to 0/wrap */
        cap = next;
    }
    uint8_t *np = realloc(b->p, cap);
    if (!np) {
        b->err = 1;
        return;
    }
    b->p = np;
    b->cap = cap;
}

static void put_bytes(Buf *b, const void *s, size_t n) {
    buf_reserve(b, n);
    if (b->err) {
        return;
    }
    /* memcpy's arguments are declared non-null, so passing a NULL source is
     * undefined even for n == 0 — and a record decoded from a zero-length
     * payload has exactly that: data == NULL, data_len == 0. Re-encoding one
     * (relate, update, compaction all re-append a loaded record) tripped UBSan.
     * Harmless on every real platform, which is why it went unnoticed until a
     * test finally encoded a NULL payload. */
    if (n) {
        memcpy(b->p + b->len, s, n);
        b->len += n;
    }
}
static void put_u8(Buf *b, uint8_t v) { put_bytes(b, &v, 1); }
static void put_u16(Buf *b, uint16_t v) {
    uint8_t t[2];
    aegis_put_u16le(t, v);
    put_bytes(b, t, 2);
}
static void put_u32(Buf *b, uint32_t v) {
    uint8_t t[4];
    aegis_put_u32le(t, v);
    put_bytes(b, t, 4);
}
static void put_u64(Buf *b, uint64_t v) {
    uint8_t t[8];
    aegis_put_u64le(t, v);
    put_bytes(b, t, 8);
}
static void put_f32(Buf *b, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    put_u32(b, u);
}
static void put_lenstr(Buf *b, const char *s, size_t n) {
    if (!s) {
        put_u32(b, NULL_LEN);
        return;
    }
    put_u32(b, (uint32_t)n);
    put_bytes(b, s, n);
}

int record_encode(const MemoryRecord *r, uint8_t **out, size_t *out_len) {
    Buf b = {0};
    /* Only a fact-bearing record needs v3 and only a derived one needs v4;
     * everything else stays at the version whose bytes are unchanged from
     * before the field existed. */
    int has_fact = r->fact.kind != FACT_NONE;
    int has_deriv = r->derivation.rule != DERIV_NONE;
    if (has_fact &&
        (r->fact.kind != FACT_OBJ_ID && r->fact.kind != FACT_OBJ_STRING)) {
        return -1; /* an unknown kind has no defined encoding */
    }
    if (has_deriv && (r->derivation.rule != DERIV_TRANSITIVE &&
                      r->derivation.rule != DERIV_SYMMETRIC &&
                      r->derivation.rule != DERIV_INVERSE)) {
        return -1; /* an unknown rule has no defined encoding */
    }
    /* A derivation without a fact would be provenance for nothing: every rule
     * in 5.3 concludes a triple, so the conclusion has to be present. Refusing
     * keeps a meaningless frame out of the log rather than durably storing one
     * no reader can interpret. */
    if (has_deriv && !has_fact) {
        return -1;
    }
    put_u8(&b, has_deriv  ? RECORD_CODEC_V4
               : has_fact ? RECORD_CODEC_V3
                          : RECORD_CODEC_V2);
    put_u64(&b, r->id);
    put_u8(&b, (uint8_t)r->type);
    put_u64(&b, r->created);
    put_u64(&b, r->updated);
    put_f32(&b, r->importance);
    put_f32(&b, r->confidence);
    put_u8(&b, (uint8_t)(r->deleted ? 1 : 0));
    put_u64(&b, r->expires_at);
    put_lenstr(&b, r->agent_id, r->agent_id ? strlen(r->agent_id) : 0);

    /* Like rel_count below, these counts are width-limited on the wire; a silent
     * truncation would desync decode (the element loops write the untruncated
     * count) and produce an undecodable, durable frame. Refuse instead. */
    if (r->tag_count > UINT16_MAX) {
        free(b.p);
        return -1;
    }
    put_u16(&b, (uint16_t)r->tag_count);
    for (size_t i = 0; i < r->tag_count; i++) {
        put_lenstr(&b, r->tags[i], strlen(r->tags[i]));
    }

    /* v2: vec_count, dim, then vec_count*dim floats (contiguous vectors). */
    if (r->vec_count > UINT32_MAX || r->embedding_dim > UINT32_MAX) {
        free(b.p);
        return -1;
    }
    put_u32(&b, (uint32_t)r->vec_count);
    put_u32(&b, (uint32_t)r->embedding_dim);
    for (size_t i = 0; i < r->vec_count * r->embedding_dim; i++) {
        put_f32(&b, r->embedding[i]);
    }

    /* The wire count is u16; truncation here would produce an undecodable frame
     * (durable data loss). qe_relate caps rel_count far below this, so tripping
     * it means a caller built a pathological record — refuse to encode it. */
    if (r->rel_count > UINT16_MAX) {
        free(b.p);
        return -1;
    }
    put_u16(&b, (uint16_t)r->rel_count);
    for (size_t i = 0; i < r->rel_count; i++) {
        put_u64(&b, r->relationships[i].from_id);
        put_u64(&b, r->relationships[i].to_id);
        put_lenstr(&b, r->relationships[i].kind,
                   r->relationships[i].kind ? strlen(r->relationships[i].kind)
                                            : 0);
    }

    /* v3 only: the fact sits between the relationships and the payload, keeping
     * the variable-length payload last as it has always been. */
    if (has_fact) {
        put_u8(&b, (uint8_t)r->fact.kind);
        put_u64(&b, r->fact.subject);
        put_lenstr(&b, r->fact.predicate,
                   r->fact.predicate ? strlen(r->fact.predicate) : 0);
        if (r->fact.kind == FACT_OBJ_ID) {
            put_u64(&b, r->fact.object_id);
        } else {
            put_lenstr(&b, r->fact.object_str,
                       r->fact.object_str ? strlen(r->fact.object_str) : 0);
        }
    }

    /* v4 only: the derivation follows the fact it explains and still precedes
     * the payload, so the variable-length payload stays last as it always has.
     * The count is bounded well below its u16 width by DERIV_MAX_PREMISES;
     * refuse rather than truncate, as every other count here does, since a
     * truncated count desyncs decode into a durable undecodable frame. */
    if (has_deriv) {
        if (r->derivation.premise_count == 0 ||
            r->derivation.premise_count > DERIV_MAX_PREMISES) {
            free(b.p);
            return -1;
        }
        put_u8(&b, (uint8_t)r->derivation.rule);
        put_u16(&b, r->derivation.depth);
        put_u16(&b, (uint16_t)r->derivation.premise_count);
        for (size_t i = 0; i < r->derivation.premise_count; i++) {
            put_u64(&b, r->derivation.premises[i]);
        }
    }

    put_u32(&b, (uint32_t)r->data_len);
    put_bytes(&b, r->data, r->data_len);

    if (b.err) {
        free(b.p);
        return -1;
    }
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
    if (c->err || c->off + n > c->len) {
        c->err = 1;
        return -1;
    }
    if (dst) {
        memcpy(dst, c->p + c->off, n);
    }
    c->off += n;
    return 0;
}
static uint8_t get_u8(Cur *c) {
    uint8_t v = 0;
    cur_take(c, &v, 1);
    return v;
}
static uint16_t get_u16(Cur *c) {
    uint8_t t[2] = {0};
    cur_take(c, t, 2);
    return aegis_get_u16le(t);
}
static uint32_t get_u32(Cur *c) {
    uint8_t t[4] = {0};
    cur_take(c, t, 4);
    return aegis_get_u32le(t);
}
static uint64_t get_u64(Cur *c) {
    uint8_t t[8] = {0};
    cur_take(c, t, 8);
    return aegis_get_u64le(t);
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
    if (was_null) {
        *was_null = 0;
    }
    if (n == NULL_LEN) {
        if (was_null) {
            *was_null = 1;
        }
        return NULL;
    }
    /* subtraction-form (off <= len invariant): cannot overflow, unlike off + n */
    if (c->err || n > c->len - c->off) {
        c->err = 1;
        return NULL;
    }
    char *s = malloc((size_t)n + 1);
    if (!s) {
        c->err = 1;
        return NULL;
    }
    memcpy(s, c->p + c->off, n);
    s[n] = '\0';
    c->off += n;
    return s;
}

int record_decode(const uint8_t *buf, size_t len, MemoryRecord *out) {
    Cur c = {buf, len, 0, 0};
    record_init(out);

    uint8_t ver = get_u8(&c);
    if (ver != 1 && ver != RECORD_CODEC_V2 && ver != RECORD_CODEC_V3 &&
        ver != RECORD_CODEC_V4) {
        goto fail; /* v1 (single vec), v2, v3 (fact), v4 (derivation) */
    }
    out->id = get_u64(&c);
    out->type = (MemoryType)get_u8(&c);
    if (out->type > MEM_SEMANTIC) {
        goto fail; /* reject a corrupt/out-of-range enum */
    }
    out->created = get_u64(&c);
    out->updated = get_u64(&c);
    out->importance = get_f32(&c);
    out->confidence = get_f32(&c);
    /* Insert validates importance/confidence into [0,1], but a corrupt log or a
     * malicious replication peer could carry a non-finite / out-of-range weight
     * that would poison ranking math (NaN comparisons are all false). Clamp back
     * to the defaults so decode is self-defending. */
    if (!isfinite(out->importance) || out->importance < 0.0F ||
        out->importance > 1.0F) {
        out->importance = 0.0F;
    }
    if (!isfinite(out->confidence) || out->confidence < 0.0F ||
        out->confidence > 1.0F) {
        out->confidence = 1.0F;
    }
    out->deleted = get_u8(&c);
    out->expires_at = get_u64(&c);

    int wasnull;
    out->agent_id = get_lenstr(&c, &wasnull);
    if (c.err) {
        goto fail;
    }

    uint16_t tc = get_u16(&c);
    if (c.err) {
        goto fail;
    }
    if (tc) {
        out->tags = calloc(tc, sizeof(char *));
        if (!out->tags) {
            goto fail;
        }
        for (uint16_t i = 0; i < tc; i++) {
            out->tags[i] = get_lenstr(&c, NULL);
            if (c.err || !out->tags[i]) {
                goto fail;
            }
            out->tag_count = (size_t)i + 1;
        }
    }

    /* v1: [dim][dim floats] (implicitly one vector). v2: [vec_count][dim]
     * [vec_count*dim floats]. */
    uint32_t vec_count = (ver >= 2) ? get_u32(&c) : 1;
    uint32_t dim = get_u32(&c);
    if (c.err) {
        goto fail;
    }
    if (ver == 1 && dim == 0) {
        vec_count = 0; /* v1 with no embedding */
    }
    if (dim && vec_count) {
        /* `vec_count`/`dim` are attacker-controlled on the decode path (a
         * replicated frame, or a tampered/corrupt log). Bound the float count
         * against the payload BEFORE any multiply: computing total*4 first would
         * overflow size_t (e.g. vec_count=dim=2^31 -> total*4 wraps to 0),
         * defeating the check and undersizing the malloc -> heap overflow.
         * Division-form checks cannot overflow (c.off <= c.len invariant). */
        size_t avail_floats = (c.len - c.off) / 4;
        if (vec_count > avail_floats / dim) {
            goto fail; /* total > payload */
        }
        size_t total = (size_t)vec_count * dim; /* <= avail_floats now */
        out->embedding = malloc(total * sizeof(float));
        if (!out->embedding) {
            goto fail;
        }
        for (size_t i = 0; i < total; i++) {
            out->embedding[i] = get_f32(&c);
            if (c.err) {
                goto fail; /* every read is in-bounds; guard anyway */
            }
        }
        out->embedding_dim = dim;
        out->vec_count = vec_count;
    }

    uint16_t rc = get_u16(&c);
    if (c.err) {
        goto fail;
    }
    for (uint16_t i = 0; i < rc; i++) {
        uint64_t from = get_u64(&c);
        uint64_t to = get_u64(&c);
        char *kind = get_lenstr(&c, &wasnull);
        if (c.err) {
            free(kind);
            goto fail;
        }
        int rv = record_add_relationship(out, from, to, kind);
        free(kind);
        if (rv) {
            goto fail;
        }
    }

    if (ver >= RECORD_CODEC_V3) {
        uint8_t fk = get_u8(&c);
        if (c.err) {
            goto fail;
        }
        /* Refuse a kind this build does not know rather than guess how many
         * bytes it occupies: guessing would desync the cursor and decode the
         * payload as garbage. A future object kind is therefore a codec bump,
         * which is exactly what the version byte is for. */
        if (fk != FACT_OBJ_ID && fk != FACT_OBJ_STRING) {
            goto fail;
        }
        uint64_t subject = get_u64(&c);
        char *pred = get_lenstr(&c, &wasnull);
        if (c.err || !pred) {
            free(pred);
            goto fail; /* a v3 frame always carries a predicate */
        }
        int rv;
        if (fk == FACT_OBJ_ID) {
            uint64_t oid = get_u64(&c);
            rv = c.err ? -1
                       : record_set_fact(out, FACT_OBJ_ID, subject, pred, oid,
                                         NULL);
        } else {
            char *obj = get_lenstr(&c, &wasnull);
            rv = (c.err || !obj) ? -1
                                 : record_set_fact(out, FACT_OBJ_STRING,
                                                   subject, pred, 0, obj);
            free(obj);
        }
        free(pred);
        if (rv != 0) {
            goto fail;
        }
    }

    if (ver >= RECORD_CODEC_V4) {
        uint8_t rule = get_u8(&c);
        uint16_t depth = get_u16(&c);
        uint16_t pc = get_u16(&c);
        if (c.err) {
            goto fail;
        }
        /* Refuse a rule this build cannot name. Unlike an unknown FactKind the
         * framing is fixed, so this could be skipped over — but a derived
         * record whose provenance is uninterpretable would be handed to a
         * caller as if it were understood, and provenance nobody can read is
         * the one thing this field exists to prevent. */
        if (rule != DERIV_TRANSITIVE && rule != DERIV_SYMMETRIC &&
            rule != DERIV_INVERSE) {
            goto fail;
        }
        if (pc == 0 || pc > DERIV_MAX_PREMISES) {
            goto fail;
        }
        uint64_t prem[DERIV_MAX_PREMISES];
        for (uint16_t i = 0; i < pc; i++) {
            prem[i] = get_u64(&c);
        }
        if (c.err ||
            record_set_derivation(out, (DerivRule)rule, depth, prem, pc) != 0) {
            goto fail;
        }
    }

    uint32_t dl = get_u32(&c);
    if (c.err || dl > c.len - c.off) {
        goto fail; /* subtraction-form: no overflow */
    }
    if (dl) {
        out->data = malloc(dl);
        if (!out->data) {
            goto fail;
        }
        memcpy(out->data, c.p + c.off, dl);
        c.off += dl;
    }
    out->data_len = dl;

    if (c.err) {
        goto fail;
    }
    return 0;
fail:
    record_free(out);
    return -1;
}