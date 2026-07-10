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
    for (size_t i = 0; i < 4; i++) r.embedding[i] = (float)(i + 1) * 0.25f;
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
#define Z(k) do { for (int _i = 0; _i < (k); _i++) B(0); } while (0)
    B(2);                       /* version 2 */
    B(1); Z(7);                 /* id = 1 (u64 LE) */
    B(0);                       /* type */
    Z(8); Z(8);                 /* created, updated */
    Z(4); Z(4);                 /* importance, confidence (f32) */
    B(0);                       /* deleted */
    Z(8);                       /* expires_at */
    B(0xFF); B(0xFF); B(0xFF); B(0xFF); /* agent_id = NULL marker */
    B(0); B(0);                 /* tag_count = 0 */
    B(0); B(0); B(0); B(0x80);  /* vec_count = 0x80000000 */
    B(0); B(0); B(0); B(0x80);  /* dim       = 0x80000000  -> total = 2^62 */
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
    for (size_t i = 0; i < 12; i++) r.embedding[i] = (float)i * 0.5f;

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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_defaults);
    RUN_TEST(test_encode_decode_roundtrip);
    RUN_TEST(test_encode_decode_with_embedding_and_agent);
    RUN_TEST(test_encode_decode_with_relationship);
    RUN_TEST(test_decode_rejects_truncated);
    RUN_TEST(test_decode_rejects_embedding_overflow);
    RUN_TEST(test_clone_is_deep);
    RUN_TEST(test_multivector_roundtrip);
    return UNITY_END();
}