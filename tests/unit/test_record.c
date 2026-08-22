/* Unit tests for MemoryRecord defaults, encode/decode round-trip, and clone. */
#include <stdlib.h>
#include <string.h>

#include "aegisdb/record.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_init_defaults(void) {
    MemoryRecord r;
    record_init(&r);
    TEST_ASSERT_EQUAL_UINT64(0, r.id);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.importance);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, r.confidence); /* documented default */
    TEST_ASSERT_NULL(r.tags);
    TEST_ASSERT_EQUAL_size_t(0, r.tag_count);
    TEST_ASSERT_NULL(r.embedding);
    TEST_ASSERT_NULL(r.data);
    record_free(&r);
}

static void test_encode_decode_roundtrip(void) {
    MemoryRecord r;
    record_init(&r);
    r.id = 42;
    r.type = MEM_EPISODIC;
    r.created = 1719400000123ull;
    r.updated = 1719400000123ull;
    r.importance = 0.7f;
    r.confidence = 0.9f;
    const char *tags[] = {"user", "preference"};
    TEST_ASSERT_EQUAL_INT(0, record_set_tags(&r, tags, 2));
    const char *payload = "User likes coffee";
    r.data = strdup(payload);
    r.data_len = strlen(payload);

    uint8_t *buf = NULL;
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&r, &buf, &len));
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_GREATER_THAN_size_t(0, len);

    MemoryRecord d;
    TEST_ASSERT_EQUAL_INT(0, record_decode(buf, len, &d));
    TEST_ASSERT_EQUAL_UINT64(42, d.id);
    TEST_ASSERT_EQUAL_INT(MEM_EPISODIC, d.type);
    TEST_ASSERT_EQUAL_UINT64(1719400000123ull, d.created);
    TEST_ASSERT_EQUAL_UINT64(1719400000123ull, d.updated);
    TEST_ASSERT_EQUAL_FLOAT(0.7f, d.importance);
    TEST_ASSERT_EQUAL_FLOAT(0.9f, d.confidence);
    TEST_ASSERT_EQUAL_size_t(2, d.tag_count);
    TEST_ASSERT_EQUAL_STRING("user", d.tags[0]);
    TEST_ASSERT_EQUAL_STRING("preference", d.tags[1]);
    TEST_ASSERT_EQUAL_size_t(strlen(payload), d.data_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, d.data, d.data_len);

    free(buf);
    record_free(&r);
    record_free(&d);
}

static void test_encode_decode_with_embedding_and_agent(void) {
    MemoryRecord r;
    record_init(&r);
    r.id = 7;
    r.type = MEM_SEMANTIC;
    r.created = r.updated = 1000;
    r.agent_id = strdup("agent-001");
    r.embedding_dim = 4;
    r.vec_count = 1;
    r.embedding = malloc(sizeof(float) * 4);
    for (size_t i = 0; i < 4; i++)
        r.embedding[i] = (float)(i + 1) * 0.25f;
    r.data = strdup("x");
    r.data_len = 1;

    uint8_t *buf = NULL;
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&r, &buf, &len));
    MemoryRecord d;
    TEST_ASSERT_EQUAL_INT(0, record_decode(buf, len, &d));

    TEST_ASSERT_EQUAL_STRING("agent-001", d.agent_id);
    TEST_ASSERT_EQUAL_size_t(4, d.embedding_dim);
    for (size_t i = 0; i < 4; i++)
        TEST_ASSERT_EQUAL_FLOAT((float)(i + 1) * 0.25f, d.embedding[i]);

    free(buf);
    record_free(&r);
    record_free(&d);
}

static void test_encode_decode_with_relationship(void) {
    MemoryRecord r;
    record_init(&r);
    r.id = 5;
    r.type = MEM_SEMANTIC;
    r.created = r.updated = 1;
    r.data = strdup("d");
    r.data_len = 1;
    TEST_ASSERT_EQUAL_INT(0, record_add_relationship(&r, 5, 9, "derived_from"));

    uint8_t *buf = NULL;
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&r, &buf, &len));
    MemoryRecord d;
    TEST_ASSERT_EQUAL_INT(0, record_decode(buf, len, &d));

    TEST_ASSERT_EQUAL_size_t(1, d.rel_count);
    TEST_ASSERT_EQUAL_UINT64(5, d.relationships[0].from_id);
    TEST_ASSERT_EQUAL_UINT64(9, d.relationships[0].to_id);
    TEST_ASSERT_EQUAL_STRING("derived_from", d.relationships[0].kind);

    free(buf);
    record_free(&r);
    record_free(&d);
}

/* A crafted frame with huge vec_count*dim must be rejected, not cause a heap
 * overflow. Before the fix, total*4 overflowed size_t so the bounds check
 * passed on a tiny payload and the fill loop wrote past a 0-sized malloc. This
 * frame is only reachable via the replication stream or a tampered log, both of
 * which hand attacker-controlled bytes to record_decode. */
static void test_decode_rejects_embedding_overflow(void) {
    uint8_t buf[128];
    size_t n = 0;
#define B(x) (buf[n++] = (uint8_t)(x))
#define Z(k)                                                                   \
    do {                                                                       \
        for (int _i = 0; _i < (k); _i++)                                       \
            B(0);                                                              \
    } while (0)
    B(2); /* version 2 */
    B(1);
    Z(7); /* id = 1 (u64 LE) */
    B(0); /* type */
    Z(8);
    Z(8); /* created, updated */
    Z(4);
    Z(4); /* importance, confidence (f32) */
    B(0); /* deleted */
    Z(8); /* expires_at */
    B(0xFF);
    B(0xFF);
    B(0xFF);
    B(0xFF); /* agent_id = NULL marker */
    B(0);
    B(0); /* tag_count = 0 */
    B(0);
    B(0);
    B(0);
    B(0x80); /* vec_count = 0x80000000 */
    B(0);
    B(0);
    B(0);
    B(0x80); /* dim       = 0x80000000  -> total = 2^62 */
    /* no float payload follows: the guard must reject before allocating */
    MemoryRecord d;
    TEST_ASSERT_EQUAL_INT(-1, record_decode(buf, n, &d));
#undef B
#undef Z
}

static void test_decode_rejects_truncated(void) {
    MemoryRecord r;
    record_init(&r);
    r.id = 1;
    r.type = MEM_EPISODIC;
    r.created = r.updated = 1;
    r.data = strdup("hello");
    r.data_len = 5;
    uint8_t *buf = NULL;
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&r, &buf, &len));

    MemoryRecord d;
    /* Feeding a truncated buffer must fail rather than read out of bounds. */
    TEST_ASSERT_EQUAL_INT(-1, record_decode(buf, len / 2, &d));

    free(buf);
    record_free(&r);
}

/* A corrupt/tampered frame carrying an out-of-range MemoryType (only 0..2 are
 * valid) must be rejected rather than decoded into an invalid enum. */
static void test_decode_rejects_bad_type(void) {
    MemoryRecord r;
    record_init(&r);
    r.id = 7;
    r.type = MEM_EPISODIC;
    const char *payload = "x";
    r.data = strdup(payload);
    r.data_len = 1;

    uint8_t *buf = NULL;
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&r, &buf, &len));
    /* Layout: ver(1) + id(8) + type(1)... -> the type byte is at offset 9. */
    TEST_ASSERT_GREATER_THAN_size_t(9, len);
    buf[9] = 5; /* not a valid MemoryType */

    MemoryRecord d;
    TEST_ASSERT_EQUAL_INT(-1, record_decode(buf, len, &d));

    free(buf);
    record_free(&r);
}

static void test_clone_is_deep(void) {
    MemoryRecord r;
    record_init(&r);
    r.id = 11;
    r.type = MEM_EPISODIC;
    r.created = r.updated = 2;
    const char *tags[] = {"a", "b"};
    record_set_tags(&r, tags, 2);
    r.data = strdup("payload");
    r.data_len = 7;

    MemoryRecord *c = record_clone(&r);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT64(r.id, c->id);
    TEST_ASSERT_EQUAL_size_t(2, c->tag_count);
    TEST_ASSERT_EQUAL_STRING("a", c->tags[0]);
    /* Distinct allocations: freeing the source must not corrupt the clone. */
    TEST_ASSERT_NOT_EQUAL(r.data, c->data);
    record_free(&r);
    /* data is an opaque length-delimited payload (not NUL-terminated), so it
     * must be compared by length — EQUAL_STRING would read past the buffer. */
    TEST_ASSERT_EQUAL_size_t(7, c->data_len);
    TEST_ASSERT_EQUAL_MEMORY("payload", c->data, c->data_len);

    record_free(c);
    free(c);
}

/* A multi-vector record (#85) round-trips through encode/decode and clone:
 * vec_count, dim, and all vec_count*dim floats are preserved. */
static void test_multivector_roundtrip(void) {
    MemoryRecord r;
    record_init(&r);
    r.id = 21;
    r.type = MEM_SEMANTIC;
    r.created = r.updated = 5;
    r.data = strdup("m");
    r.data_len = 1;
    r.embedding_dim = 4;
    r.vec_count = 3; /* three 4-D vectors, contiguous */
    r.embedding = malloc(sizeof(float) * 12);
    for (size_t i = 0; i < 12; i++)
        r.embedding[i] = (float)i * 0.5f;

    uint8_t *buf = NULL;
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&r, &buf, &len));
    MemoryRecord d;
    TEST_ASSERT_EQUAL_INT(0, record_decode(buf, len, &d));
    TEST_ASSERT_EQUAL_size_t(4, d.embedding_dim);
    TEST_ASSERT_EQUAL_size_t(3, d.vec_count);
    for (size_t i = 0; i < 12; i++)
        TEST_ASSERT_EQUAL_FLOAT((float)i * 0.5f, d.embedding[i]);
    free(buf);
    record_free(&d);

    MemoryRecord *cl = record_clone(&r);
    TEST_ASSERT_NOT_NULL(cl);
    TEST_ASSERT_EQUAL_size_t(3, cl->vec_count);
    TEST_ASSERT_EQUAL_size_t(4, cl->embedding_dim);
    for (size_t i = 0; i < 12; i++)
        TEST_ASSERT_EQUAL_FLOAT((float)i * 0.5f, cl->embedding[i]);
    record_free(cl);
    free(cl);
    record_free(&r);
}

/* A record whose rel_count exceeds the u16 wire field must be refused by
 * record_encode rather than silently truncated (which would emit a frame that
 * record_decode cannot parse -> durable data loss). The relationship array is
 * built directly (not via record_add_relationship) to avoid O(n^2) reallocs. */
static void test_encode_rejects_relationship_overflow(void) {
    MemoryRecord r;
    record_init(&r);
    r.id = 1;
    r.type = MEM_SEMANTIC;
    r.created = r.updated = 1;
    r.data = strdup("d");
    r.data_len = 1;
    size_t n = 65536 + 1;                              /* one past UINT16_MAX */
    r.relationships = calloc(n, sizeof(Relationship)); /* kind=NULL, ids=0 */
    TEST_ASSERT_NOT_NULL(r.relationships);
    r.rel_count = n;

    uint8_t *buf = NULL;
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(-1, record_encode(&r, &buf, &len));
    TEST_ASSERT_NULL(buf);

    record_free(&r); /* frees the (NULL-kind) relationship array */
}

/* record_clone must refuse a record whose vec_count*dim (or *sizeof(float))
 * would overflow rather than under-allocate and overflow the heap on memcpy. */
static void test_clone_rejects_embedding_overflow(void) {
    MemoryRecord r;
    record_init(&r);
    r.id = 1;
    r.type = MEM_SEMANTIC;
    r.created = r.updated = 1;
    r.embedding =
        malloc(sizeof(float)); /* real 1-float buffer; guard trips first */
    TEST_ASSERT_NOT_NULL(r.embedding);
    r.embedding[0] = 1.0f;
    r.vec_count = (size_t)-1 / 2; /* vec_count * dim overflows size_t */
    r.embedding_dim = 4;

    MemoryRecord *c = record_clone(&r);
    TEST_ASSERT_NULL(c); /* refused, no allocation/overflow */

    record_free(
        &r); /* frees the 1-float buffer (record_free ignores vec_count) */
}

/* ---- typed facts, codec v3 (ROADMAP 5.2) -------------------------------- */

/* THE compatibility test. A record with no fact must encode to exactly the
 * bytes the v2 codec produced before facts existed — not merely round-trip.
 * These bytes were captured from the pre-5.2 encoder for the record built
 * below; if this fails, an existing log has become unreadable or a replica of a
 * different version has started disagreeing, and neither shows up as a
 * round-trip failure. */
static void test_factless_record_encodes_byte_identical_v2(void) {
    static const uint8_t GOLDEN_V2[] = {
        0x02, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x61,
        0x63, 0x6d, 0x65, 0x02, 0x00, 0x05, 0x00, 0x00, 0x00, 0x61, 0x6c, 0x70,
        0x68, 0x61, 0x04, 0x00, 0x00, 0x00, 0x62, 0x65, 0x74, 0x61, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0a, 0x00, 0x00, 0x00, 0x73, 0x75, 0x70, 0x65, 0x72, 0x73, 0x65, 0x64,
        0x65, 0x73, 0x02, 0x00, 0x00, 0x00, 0x68, 0x69,
    };
    MemoryRecord r;
    record_init(&r);
    r.id = 7;
    r.type = MEM_EPISODIC;
    r.created = 100;
    r.updated = 200;
    r.importance = 0.5F;
    r.confidence = 1.0F;
    const char *tags[] = {"alpha", "beta"};
    TEST_ASSERT_EQUAL_INT(0, record_set_tags(&r, tags, 2));
    r.agent_id = strdup("acme");
    TEST_ASSERT_EQUAL_INT(0, record_add_relationship(&r, 7, 9, "supersedes"));
    r.data = strdup("hi");
    r.data_len = 2;

    uint8_t *buf = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&r, &buf, &n));
    TEST_ASSERT_EQUAL_size_t(sizeof(GOLDEN_V2), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(GOLDEN_V2, buf, sizeof(GOLDEN_V2));
    TEST_ASSERT_EQUAL_UINT8(2, buf[0]); /* still announces itself as v2 */
    free(buf);
    record_free(&r);
}

/* A frame written before facts existed decodes with no fact, rather than
 * failing or inventing one. */
static void test_v2_frame_decodes_with_no_fact(void) {
    MemoryRecord src;
    record_init(&src);
    src.id = 3;
    src.type = MEM_SEMANTIC;
    src.data = strdup("plain");
    src.data_len = 5;
    uint8_t *buf = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&src, &buf, &n));
    TEST_ASSERT_EQUAL_UINT8(2, buf[0]);

    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(0, record_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_INT(FACT_NONE, out.fact.kind);
    TEST_ASSERT_NULL(out.fact.predicate);
    TEST_ASSERT_NULL(out.fact.object_str);
    free(buf);
    record_free(&out);
    record_free(&src);
}

static void test_fact_id_object_roundtrip(void) {
    MemoryRecord src;
    record_init(&src);
    src.id = 11;
    src.type = MEM_SEMANTIC;
    src.data = strdup("hnsw.c is part of the storage layer");
    src.data_len = strlen("hnsw.c is part of the storage layer");
    TEST_ASSERT_EQUAL_INT(
        0, record_set_fact(&src, FACT_OBJ_ID, 42, "part_of", 99, NULL));

    uint8_t *buf = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&src, &buf, &n));
    TEST_ASSERT_EQUAL_UINT8(3, buf[0]); /* a fact is what makes it v3 */

    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(0, record_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_INT(FACT_OBJ_ID, out.fact.kind);
    TEST_ASSERT_EQUAL_UINT64(42, out.fact.subject);
    TEST_ASSERT_EQUAL_STRING("part_of", out.fact.predicate);
    TEST_ASSERT_EQUAL_UINT64(99, out.fact.object_id);
    TEST_ASSERT_NULL(out.fact.object_str);
    /* the payload still decodes, i.e. the new fields did not desync the cursor */
    TEST_ASSERT_EQUAL_size_t(src.data_len, out.data_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(src.data, out.data, out.data_len));
    free(buf);
    record_free(&out);
    record_free(&src);
}

static void test_fact_string_object_roundtrip(void) {
    MemoryRecord src;
    record_init(&src);
    src.id = 12;
    src.type = MEM_SEMANTIC;
    src.data = strdup("the recall hook defaults to embedding_mode=none");
    src.data_len = strlen("the recall hook defaults to embedding_mode=none");
    TEST_ASSERT_EQUAL_INT(0, record_set_fact(&src, FACT_OBJ_STRING, 42,
                                             "defaults_to", 0, "none"));

    uint8_t *buf = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&src, &buf, &n));
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(0, record_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_INT(FACT_OBJ_STRING, out.fact.kind);
    TEST_ASSERT_EQUAL_STRING("defaults_to", out.fact.predicate);
    TEST_ASSERT_EQUAL_STRING("none", out.fact.object_str);
    TEST_ASSERT_EQUAL_UINT64(0, out.fact.object_id);
    TEST_ASSERT_EQUAL_size_t(src.data_len, out.data_len);
    free(buf);
    record_free(&out);
    record_free(&src);
}

/* An empty-string object is a real value, distinct from "no object". */
static void test_fact_empty_string_object(void) {
    MemoryRecord src;
    record_init(&src);
    src.id = 13;
    TEST_ASSERT_EQUAL_INT(
        0, record_set_fact(&src, FACT_OBJ_STRING, 1, "equals", 0, ""));
    uint8_t *buf = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&src, &buf, &n));
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(0, record_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_INT(FACT_OBJ_STRING, out.fact.kind);
    TEST_ASSERT_NOT_NULL(out.fact.object_str);
    TEST_ASSERT_EQUAL_STRING("", out.fact.object_str);
    free(buf);
    record_free(&out);
    record_free(&src);
}

/* A record decoded from a zero-length payload has data == NULL, and anything
 * that re-appends it (relate, update, compaction) encodes it again. That path
 * fed a NULL source to memcpy — undefined even for n == 0, and invisible until
 * something actually encoded a NULL payload. */
static void test_encode_null_payload_roundtrips(void) {
    MemoryRecord r;
    record_init(&r);
    r.id = 21;
    r.type = MEM_SEMANTIC;
    /* data left NULL, data_len 0 — exactly what decode produces for dl == 0 */
    uint8_t *buf = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&r, &buf, &n));
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(0, record_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_size_t(0, out.data_len);
    TEST_ASSERT_NULL(out.data);
    /* and again, from the decoded record: this is the real-world shape */
    uint8_t *buf2 = NULL;
    size_t n2 = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&out, &buf2, &n2));
    TEST_ASSERT_EQUAL_size_t(n, n2);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(buf, buf2, n);
    free(buf2);
    free(buf);
    record_free(&out);
    record_free(&r);
}

/* A kind this build does not know must be refused, not guessed at: guessing its
 * width would desync the cursor and decode the payload as garbage. */
static void test_decode_rejects_unknown_fact_kind(void) {
    MemoryRecord src;
    record_init(&src);
    src.id = 14;
    src.data = strdup("payload");
    src.data_len = 7;
    TEST_ASSERT_EQUAL_INT(
        0, record_set_fact(&src, FACT_OBJ_ID, 1, "part_of", 2, NULL));
    uint8_t *buf = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&src, &buf, &n));

    /* For this record — no agent_id, no tags, no vectors, no relationships —
     * the fact-kind byte sits at a fixed offset:
     *   ver 1 + id 8 + type 1 + created 8 + updated 8 + importance 4 +
     *   confidence 4 + deleted 1 + expires 8 + agent(NULL marker) 4 +
     *   tag_count 2 + vec_count 4 + dim 4 + rel_count 2  =  59
     * Asserting the byte is what we expect first, so a format change fails here
     * loudly instead of silently testing the wrong byte. */
    const size_t KIND_OFF = 59;
    TEST_ASSERT_TRUE(n > KIND_OFF);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FACT_OBJ_ID, buf[KIND_OFF]);

    buf[KIND_OFF] = 99; /* not a known FactKind */
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(-1, record_decode(buf, n, &out));

    buf[KIND_OFF] = (uint8_t)FACT_OBJ_ID; /* restored: decodes again */
    TEST_ASSERT_EQUAL_INT(0, record_decode(buf, n, &out));
    record_free(&out);
    free(buf);
    record_free(&src);
}

static void test_set_fact_validates_and_clears(void) {
    MemoryRecord r;
    record_init(&r);
    /* a predicate is required */
    TEST_ASSERT_EQUAL_INT(-1,
                          record_set_fact(&r, FACT_OBJ_ID, 1, NULL, 2, NULL));
    TEST_ASSERT_EQUAL_INT(-1, record_set_fact(&r, FACT_OBJ_ID, 1, "", 2, NULL));
    /* a string object is required for FACT_OBJ_STRING */
    TEST_ASSERT_EQUAL_INT(
        -1, record_set_fact(&r, FACT_OBJ_STRING, 1, "p", 0, NULL));
    /* an unknown kind is refused */
    TEST_ASSERT_EQUAL_INT(-1,
                          record_set_fact(&r, (FactKind)7, 1, "p", 0, NULL));
    TEST_ASSERT_EQUAL_INT(FACT_NONE, r.fact.kind);

    /* set, then replace, then clear — no leaks, and the old strings go */
    TEST_ASSERT_EQUAL_INT(
        0, record_set_fact(&r, FACT_OBJ_STRING, 1, "first", 0, "a"));
    TEST_ASSERT_EQUAL_INT(
        0, record_set_fact(&r, FACT_OBJ_STRING, 1, "second", 0, "b"));
    TEST_ASSERT_EQUAL_STRING("second", r.fact.predicate);
    TEST_ASSERT_EQUAL_STRING("b", r.fact.object_str);
    TEST_ASSERT_EQUAL_INT(0, record_set_fact(&r, FACT_NONE, 0, NULL, 0, NULL));
    TEST_ASSERT_EQUAL_INT(FACT_NONE, r.fact.kind);
    TEST_ASSERT_NULL(r.fact.predicate);
    TEST_ASSERT_NULL(r.fact.object_str);
    record_free(&r);
}

/* Switching object kinds must not leave the old object string behind. */
static void test_set_fact_switching_kinds_releases_object(void) {
    MemoryRecord r;
    record_init(&r);
    TEST_ASSERT_EQUAL_INT(
        0, record_set_fact(&r, FACT_OBJ_STRING, 1, "p", 0, "literal"));
    TEST_ASSERT_EQUAL_INT(0,
                          record_set_fact(&r, FACT_OBJ_ID, 1, "p", 55, NULL));
    TEST_ASSERT_EQUAL_INT(FACT_OBJ_ID, r.fact.kind);
    TEST_ASSERT_NULL(r.fact.object_str);
    TEST_ASSERT_EQUAL_UINT64(55, r.fact.object_id);
    record_free(&r);
}

static void test_clone_copies_the_fact(void) {
    MemoryRecord src;
    record_init(&src);
    src.id = 15;
    TEST_ASSERT_EQUAL_INT(0, record_set_fact(&src, FACT_OBJ_STRING, 42,
                                             "defaults_to", 0, "none"));
    MemoryRecord *cp = record_clone(&src);
    TEST_ASSERT_NOT_NULL(cp);
    TEST_ASSERT_EQUAL_INT(FACT_OBJ_STRING, cp->fact.kind);
    TEST_ASSERT_EQUAL_STRING("defaults_to", cp->fact.predicate);
    TEST_ASSERT_EQUAL_STRING("none", cp->fact.object_str);
    /* deep, not aliased */
    TEST_ASSERT_TRUE(cp->fact.predicate != src.fact.predicate);
    TEST_ASSERT_TRUE(cp->fact.object_str != src.fact.object_str);
    record_free(cp);
    free(cp);
    record_free(&src);
}

/* ---- derivation, codec v4 (ROADMAP 5.3) --------------------------------- */

/* THE compatibility test for v4, in the shape v3's used. A record that carries
 * a fact but no derivation must encode to exactly the bytes the v3 codec
 * produced before derivations existed — not merely round-trip. These bytes were
 * captured from the pre-5.3 encoder for the record built below and diffed
 * against it; a round-trip test would pass while the format silently moved. */
static void test_underived_record_encodes_byte_identical_v3(void) {
    static const uint8_t GOLDEN_V3[] = {
        0x03, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x61,
        0x63, 0x6d, 0x65, 0x01, 0x00, 0x05, 0x00, 0x00, 0x00, 0x61, 0x6c, 0x70,
        0x68, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x64, 0x65, 0x72, 0x69,
        0x76, 0x65, 0x64, 0x5f, 0x66, 0x72, 0x6f, 0x6d, 0x01, 0x0c, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x70, 0x61, 0x72,
        0x74, 0x5f, 0x6f, 0x66, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x68, 0x69,
    };
    MemoryRecord r;
    record_init(&r);
    r.id = 7;
    r.type = MEM_SEMANTIC;
    r.created = 100;
    r.updated = 200;
    r.importance = 0.5F;
    r.confidence = 1.0F;
    const char *tags[] = {"alpha"};
    TEST_ASSERT_EQUAL_INT(0, record_set_tags(&r, tags, 1));
    r.agent_id = strdup("acme");
    TEST_ASSERT_EQUAL_INT(0, record_add_relationship(&r, 7, 9, "derived_from"));
    TEST_ASSERT_EQUAL_INT(
        0, record_set_fact(&r, FACT_OBJ_ID, 12, "part_of", 56, NULL));
    r.data = strdup("hi");
    r.data_len = 2;

    uint8_t *buf = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&r, &buf, &n));
    TEST_ASSERT_EQUAL_size_t(sizeof(GOLDEN_V3), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(GOLDEN_V3, buf, sizeof(GOLDEN_V3));
    TEST_ASSERT_EQUAL_UINT8(3, buf[0]); /* still announces itself as v3 */
    free(buf);
    record_free(&r);
}

/* A fact-less, derivation-less record is still v2 — v4 must not disturb the
 * older step of the same ladder. */
static void test_plain_record_is_still_v2(void) {
    MemoryRecord r;
    record_init(&r);
    r.id = 3;
    r.data = strdup("x");
    r.data_len = 1;
    uint8_t *buf = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&r, &buf, &n));
    TEST_ASSERT_EQUAL_UINT8(RECORD_CODEC_V2, buf[0]);
    free(buf);
    record_free(&r);
}

static void test_derived_record_roundtrips_as_v4(void) {
    const uint64_t prem[] = {88, 91};
    MemoryRecord src;
    record_init(&src);
    src.id = 21;
    src.type = MEM_SEMANTIC;
    src.confidence = 0.49F;
    src.data = strdup("a part_of c");
    src.data_len = 11;
    TEST_ASSERT_EQUAL_INT(
        0, record_set_fact(&src, FACT_OBJ_ID, 12, "part_of", 56, NULL));
    TEST_ASSERT_EQUAL_INT(
        0, record_set_derivation(&src, DERIV_TRANSITIVE, 2, prem, 2));

    uint8_t *buf = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&src, &buf, &n));
    TEST_ASSERT_EQUAL_UINT8(RECORD_CODEC_V4, buf[0]);

    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(0, record_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_INT(DERIV_TRANSITIVE, out.derivation.rule);
    TEST_ASSERT_EQUAL_UINT16(2, out.derivation.depth);
    TEST_ASSERT_EQUAL_size_t(2, out.derivation.premise_count);
    TEST_ASSERT_EQUAL_UINT64(88, out.derivation.premises[0]);
    TEST_ASSERT_EQUAL_UINT64(91, out.derivation.premises[1]);
    /* the fact it explains survives alongside it */
    TEST_ASSERT_EQUAL_INT(FACT_OBJ_ID, out.fact.kind);
    TEST_ASSERT_EQUAL_STRING("part_of", out.fact.predicate);
    TEST_ASSERT_EQUAL_size_t(11, out.data_len);

    record_free(&out);
    free(buf);
    record_free(&src);
}

/* A v3 frame predates derivations and must decode as underived, not fail and
 * not invent one. */
static void test_v3_frame_decodes_with_no_derivation(void) {
    MemoryRecord src;
    record_init(&src);
    src.id = 5;
    src.data = strdup("p");
    src.data_len = 1;
    TEST_ASSERT_EQUAL_INT(
        0, record_set_fact(&src, FACT_OBJ_STRING, 1, "defaults_to", 0, "none"));
    uint8_t *buf = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&src, &buf, &n));
    TEST_ASSERT_EQUAL_UINT8(RECORD_CODEC_V3, buf[0]);

    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(0, record_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_INT(DERIV_NONE, out.derivation.rule);
    TEST_ASSERT_NULL(out.derivation.premises);
    TEST_ASSERT_EQUAL_size_t(0, out.derivation.premise_count);
    record_free(&out);
    free(buf);
    record_free(&src);
}

/* A rule this build cannot name is refused rather than handed over as an
 * uninterpretable provenance claim. */
static void test_decode_rejects_unknown_deriv_rule(void) {
    const uint64_t prem[] = {2};
    MemoryRecord src;
    record_init(&src);
    src.id = 14;
    src.data = strdup("payload");
    src.data_len = 7;
    TEST_ASSERT_EQUAL_INT(
        0, record_set_fact(&src, FACT_OBJ_ID, 1, "part_of", 2, NULL));
    TEST_ASSERT_EQUAL_INT(
        0, record_set_derivation(&src, DERIV_SYMMETRIC, 1, prem, 1));
    uint8_t *buf = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&src, &buf, &n));

    /* Same fixed-offset reasoning as the fact-kind test: no agent_id, tags,
     * vectors or relationships, so the fact block starts at 59 and occupies
     * kind 1 + subject 8 + predlen 4 + "part_of" 7 + object_id 8 = 28. The rule
     * byte therefore sits at 87. Asserted before it is corrupted, so a format
     * change fails loudly here rather than testing the wrong byte. */
    const size_t RULE_OFF = 87;
    TEST_ASSERT_TRUE(n > RULE_OFF);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)DERIV_SYMMETRIC, buf[RULE_OFF]);

    buf[RULE_OFF] = 99; /* not a known DerivRule */
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(-1, record_decode(buf, n, &out));

    buf[RULE_OFF] = (uint8_t)DERIV_SYMMETRIC; /* restored: decodes again */
    TEST_ASSERT_EQUAL_INT(0, record_decode(buf, n, &out));
    record_free(&out);
    free(buf);
    record_free(&src);
}

/* A premise count past the cap is refused on the way in, so a crafted frame
 * cannot make the decoder overrun its fixed-size premise buffer. */
static void test_decode_rejects_premise_count_past_cap(void) {
    const uint64_t prem[] = {2};
    MemoryRecord src;
    record_init(&src);
    src.id = 14;
    src.data = strdup("payload");
    src.data_len = 7;
    TEST_ASSERT_EQUAL_INT(
        0, record_set_fact(&src, FACT_OBJ_ID, 1, "part_of", 2, NULL));
    TEST_ASSERT_EQUAL_INT(
        0, record_set_derivation(&src, DERIV_SYMMETRIC, 1, prem, 1));
    uint8_t *buf = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, record_encode(&src, &buf, &n));

    const size_t COUNT_OFF = 87 + 1 + 2; /* rule, depth, then the u16 count */
    TEST_ASSERT_TRUE(n > COUNT_OFF + 1);
    TEST_ASSERT_EQUAL_UINT8(1, buf[COUNT_OFF]);
    buf[COUNT_OFF] = DERIV_MAX_PREMISES + 1;
    MemoryRecord out;
    TEST_ASSERT_EQUAL_INT(-1, record_decode(buf, n, &out));
    buf[COUNT_OFF] = 0; /* zero premises is not a derivation either */
    TEST_ASSERT_EQUAL_INT(-1, record_decode(buf, n, &out));
    free(buf);
    record_free(&src);
}

/* A derivation with no fact is provenance for nothing: every 5.3 rule concludes
 * a triple, so refusing keeps an uninterpretable frame out of the log. */
static void test_encode_refuses_derivation_without_fact(void) {
    const uint64_t prem[] = {1};
    MemoryRecord r;
    record_init(&r);
    r.id = 4;
    r.data = strdup("x");
    r.data_len = 1;
    TEST_ASSERT_EQUAL_INT(0,
                          record_set_derivation(&r, DERIV_INVERSE, 0, prem, 1));
    uint8_t *buf = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(-1, record_encode(&r, &buf, &n));
    record_free(&r);
}

static void test_set_derivation_validates_and_clears(void) {
    const uint64_t prem[] = {1, 2, 3};
    uint64_t many[DERIV_MAX_PREMISES + 1];
    for (size_t i = 0; i < DERIV_MAX_PREMISES + 1; i++) {
        many[i] = i + 1;
    }
    MemoryRecord r;
    record_init(&r);
    /* premises are required, and bounded */
    TEST_ASSERT_EQUAL_INT(
        -1, record_set_derivation(&r, DERIV_TRANSITIVE, 0, NULL, 0));
    TEST_ASSERT_EQUAL_INT(
        -1, record_set_derivation(&r, DERIV_TRANSITIVE, 0, prem, 0));
    TEST_ASSERT_EQUAL_INT(-1,
                          record_set_derivation(&r, DERIV_TRANSITIVE, 0, many,
                                                DERIV_MAX_PREMISES + 1));
    /* an unknown rule is refused */
    TEST_ASSERT_EQUAL_INT(-1,
                          record_set_derivation(&r, (DerivRule)7, 0, prem, 1));
    TEST_ASSERT_EQUAL_INT(DERIV_NONE, r.derivation.rule);

    /* set, then replace, then clear — the old array goes each time */
    TEST_ASSERT_EQUAL_INT(
        0, record_set_derivation(&r, DERIV_TRANSITIVE, 1, prem, 3));
    TEST_ASSERT_EQUAL_size_t(3, r.derivation.premise_count);
    TEST_ASSERT_EQUAL_INT(0,
                          record_set_derivation(&r, DERIV_INVERSE, 2, prem, 1));
    TEST_ASSERT_EQUAL_INT(DERIV_INVERSE, r.derivation.rule);
    TEST_ASSERT_EQUAL_size_t(1, r.derivation.premise_count);
    TEST_ASSERT_EQUAL_UINT16(2, r.derivation.depth);
    TEST_ASSERT_EQUAL_INT(0, record_set_derivation(&r, DERIV_NONE, 0, NULL, 0));
    TEST_ASSERT_EQUAL_INT(DERIV_NONE, r.derivation.rule);
    TEST_ASSERT_NULL(r.derivation.premises);
    record_free(&r);
}

static void test_clone_preserves_derivation(void) {
    const uint64_t prem[] = {88, 91};
    MemoryRecord src;
    record_init(&src);
    src.id = 9;
    src.data = strdup("d");
    src.data_len = 1;
    TEST_ASSERT_EQUAL_INT(
        0, record_set_fact(&src, FACT_OBJ_ID, 1, "part_of", 2, NULL));
    TEST_ASSERT_EQUAL_INT(
        0, record_set_derivation(&src, DERIV_TRANSITIVE, 3, prem, 2));

    MemoryRecord *c = record_clone(&src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(DERIV_TRANSITIVE, c->derivation.rule);
    TEST_ASSERT_EQUAL_UINT16(3, c->derivation.depth);
    TEST_ASSERT_EQUAL_size_t(2, c->derivation.premise_count);
    TEST_ASSERT_EQUAL_UINT64(91, c->derivation.premises[1]);
    /* a deep copy: freeing the source leaves the clone intact */
    TEST_ASSERT_TRUE(c->derivation.premises != src.derivation.premises);
    record_free(&src);
    TEST_ASSERT_EQUAL_UINT64(88, c->derivation.premises[0]);
    record_free(c);
    free(c);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_defaults);
    RUN_TEST(test_encode_decode_roundtrip);
    RUN_TEST(test_encode_decode_with_embedding_and_agent);
    RUN_TEST(test_encode_decode_with_relationship);
    RUN_TEST(test_decode_rejects_truncated);
    RUN_TEST(test_decode_rejects_bad_type);
    RUN_TEST(test_decode_rejects_embedding_overflow);
    RUN_TEST(test_encode_rejects_relationship_overflow);
    RUN_TEST(test_clone_rejects_embedding_overflow);
    RUN_TEST(test_clone_is_deep);
    RUN_TEST(test_multivector_roundtrip);
    RUN_TEST(test_factless_record_encodes_byte_identical_v2);
    RUN_TEST(test_v2_frame_decodes_with_no_fact);
    RUN_TEST(test_fact_id_object_roundtrip);
    RUN_TEST(test_fact_string_object_roundtrip);
    RUN_TEST(test_fact_empty_string_object);
    RUN_TEST(test_encode_null_payload_roundtrips);
    RUN_TEST(test_decode_rejects_unknown_fact_kind);
    RUN_TEST(test_underived_record_encodes_byte_identical_v3);
    RUN_TEST(test_plain_record_is_still_v2);
    RUN_TEST(test_derived_record_roundtrips_as_v4);
    RUN_TEST(test_v3_frame_decodes_with_no_derivation);
    RUN_TEST(test_decode_rejects_unknown_deriv_rule);
    RUN_TEST(test_decode_rejects_premise_count_past_cap);
    RUN_TEST(test_encode_refuses_derivation_without_fact);
    RUN_TEST(test_set_derivation_validates_and_clears);
    RUN_TEST(test_clone_preserves_derivation);
    RUN_TEST(test_set_fact_validates_and_clears);
    RUN_TEST(test_set_fact_switching_kinds_releases_object);
    RUN_TEST(test_clone_copies_the_fact);
    return UNITY_END();
}