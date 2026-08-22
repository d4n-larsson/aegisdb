/* Unit tests for the predicate registry — ROADMAP 5.2.
 *
 * The registry's job is to make a typo in a vocabulary file fail loudly at
 * startup instead of becoming a rule that never fires, so most of these tests
 * are rejection cases: each one is a mistake someone will actually make. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/fact_index.h"
#include "aegisdb/predicate_registry.h"
#include "unity.h"

static char g_path[256];

void setUp(void) {
    snprintf(g_path, sizeof(g_path), "/tmp/aegis_reg_%d_%ld.json",
             (int)getpid(), (long)random());
}

void tearDown(void) { unlink(g_path); }

/* Write `json` to the temp path and try to load it. Returns the registry or
 * NULL, with the reason in `err`. */
static PredicateRegistry *load(const char *json, char *err, size_t errlen) {
    FILE *fh = fopen(g_path, "wb");
    TEST_ASSERT_NOT_NULL(fh);
    fwrite(json, 1, strlen(json), fh);
    fclose(fh);
    return predicate_registry_load(g_path, err, errlen);
}

/* Assert the file is rejected, and that the message mentions `needle` — a
 * message that does not name the culprit is not actionable. */
static void reject(const char *json, const char *needle) {
    char err[256] = "";
    PredicateRegistry *r = load(json, err, sizeof err);
    if (r) {
        predicate_registry_free(r);
        TEST_FAIL_MESSAGE("expected the registry to be rejected");
    }
    TEST_ASSERT_TRUE_MESSAGE(err[0] != '\0', "a rejection must explain itself");
    if (needle) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, needle), err);
    }
}

/* ---- the happy path ---------------------------------------------------- */

static void test_loads_a_valid_registry(void) {
    char err[256] = "";
    PredicateRegistry *r = load(
        "{\n"
        "  \"defaults_to\":    {\"object\": \"string\", \"cardinality\": "
        "\"one\"},\n"
        "  \"part_of\":        {\"object\": \"id\", \"transitive\": true,\n"
        "                       \"inverse_of\": \"contains\"},\n"
        "  \"contains\":       {\"object\": \"id\", \"inverse_of\": "
        "\"part_of\"},\n"
        "  \"conflicts_with\": {\"object\": \"id\", \"symmetric\": true},\n"
        "  \"described_as\":   {\"object\": \"string\",\n"
        "                       \"mutex_with\": [\"defaults_to\"]}\n"
        "}\n",
        err, sizeof err);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, err);
    TEST_ASSERT_EQUAL_size_t(5, predicate_registry_count(r));

    const PredicateSpec *s = predicate_registry_get(r, "defaults_to");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_INT(PRED_OBJ_STRING, s->object);
    TEST_ASSERT_EQUAL_INT(1, s->single_valued);
    TEST_ASSERT_EQUAL_INT(0, s->symmetric);

    s = predicate_registry_get(r, "part_of");
    TEST_ASSERT_EQUAL_INT(PRED_OBJ_ID, s->object);
    TEST_ASSERT_EQUAL_INT(1, s->transitive);
    TEST_ASSERT_EQUAL_STRING("contains", s->inverse_of);
    TEST_ASSERT_EQUAL_INT(0, s->single_valued); /* defaults to many */

    s = predicate_registry_get(r, "described_as");
    TEST_ASSERT_EQUAL_size_t(1, s->mutex_count);
    TEST_ASSERT_EQUAL_STRING("defaults_to", s->mutex_with[0]);

    /* an undeclared predicate is simply absent */
    TEST_ASSERT_NULL(predicate_registry_get(r, "invented"));

    /* Two specs held at once must not alias — the reason the lookup returns a
     * pointer into the table rather than into a shared scratch view. */
    const PredicateSpec *a = predicate_registry_get(r, "part_of");
    const PredicateSpec *b = predicate_registry_get(r, "contains");
    TEST_ASSERT_EQUAL_STRING("part_of", a->name);
    TEST_ASSERT_EQUAL_STRING("contains", b->name);
    TEST_ASSERT_TRUE(a != b);

    predicate_registry_free(r);
}

/* ---- what it enforces at write time ----------------------------------- */

static void test_check_enforces_membership_and_object_kind(void) {
    char err[256] = "";
    PredicateRegistry *r = load("{\"defaults_to\": {\"object\": \"string\"},\n"
                                " \"part_of\": {\"object\": \"id\"}}",
                                err, sizeof err);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, err);

    TEST_ASSERT_EQUAL_INT(0, predicate_registry_check(r, "defaults_to",
                                                      FACT_OBJ_STRING, err,
                                                      sizeof err));
    TEST_ASSERT_EQUAL_INT(0, predicate_registry_check(r, "part_of", FACT_OBJ_ID,
                                                      err, sizeof err));

    /* right predicate, wrong object shape */
    TEST_ASSERT_EQUAL_INT(-1, predicate_registry_check(r, "defaults_to",
                                                       FACT_OBJ_ID, err,
                                                       sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "defaults_to"));
    TEST_ASSERT_EQUAL_INT(-1, predicate_registry_check(r, "part_of",
                                                       FACT_OBJ_STRING, err,
                                                       sizeof err));
    /* not declared at all */
    TEST_ASSERT_EQUAL_INT(-1, predicate_registry_check(r, "invented",
                                                       FACT_OBJ_STRING, err,
                                                       sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "invented"));
    predicate_registry_free(r);
}

/* No registry means no vocabulary, which must accept everything rather than
 * nothing — a server that never opted in cannot be broken by this feature. */
static void test_null_registry_allows_everything(void) {
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(0, predicate_registry_check(NULL, "anything",
                                                      FACT_OBJ_STRING, err,
                                                      sizeof err));
    TEST_ASSERT_EQUAL_size_t(0, predicate_registry_count(NULL));
    TEST_ASSERT_NULL(predicate_registry_get(NULL, "anything"));
    predicate_registry_free(NULL);
}

/* ---- rejections -------------------------------------------------------- */

static void test_rejects_malformed_files(void) {
    reject("not json at all", "valid JSON");
    reject("[1, 2, 3]", "JSON object");
    reject("{}", "no predicates");
}

static void test_rejects_a_missing_or_bad_object_kind(void) {
    /* Required, not defaulted: a predicate that took either an id or a literal
     * would make a (predicate, object) lookup mean two different things. */
    reject("{\"p\": {}}", "\"object\" is required");
    reject("{\"p\": {\"cardinality\": \"one\"}}", "\"object\" is required");
    reject("{\"p\": {\"object\": \"number\"}}", "number");
    reject("{\"p\": {\"object\": 1}}", "\"object\" is required");
}

static void test_rejects_unknown_keys(void) {
    /* The failure this exists for: a misspelt property would otherwise be a
     * rule that silently does not apply. */
    reject("{\"p\": {\"object\": \"id\", \"transative\": true}}", "transative");
    reject("{\"p\": {\"object\": \"id\", \"inverse\": \"q\"}}", "unknown key");
}

static void test_rejects_bad_scalars(void) {
    reject("{\"p\": {\"object\": \"id\", \"symmetric\": \"yes\"}}",
           "true or false");
    reject("{\"p\": {\"object\": \"id\", \"cardinality\": \"single\"}}",
           "cardinality must be");
    reject("{\"p\": {\"object\": \"id\", \"mutex_with\": \"q\"}}",
           "must be an array");
}

/* Symmetry and transitivity relate a record to a record, so a literal-valued
 * predicate cannot have either — a rule that could never fire. */
static void test_rejects_relational_properties_on_a_literal(void) {
    reject("{\"p\": {\"object\": \"string\", \"symmetric\": true}}",
           "relate one record");
    reject("{\"p\": {\"object\": \"string\", \"transitive\": true}}",
           "relate one record");
}

static void test_rejects_dangling_and_one_sided_references(void) {
    reject("{\"p\": {\"object\": \"id\", \"inverse_of\": \"ghost\"}}",
           "not declared");
    /* One-sided: 5.3 would derive in one direction only, which reads as a bug
     * in the reasoner rather than a typo in the file. */
    reject("{\"p\": {\"object\": \"id\", \"inverse_of\": \"q\"},\n"
           " \"q\": {\"object\": \"id\"}}",
           "not mutual");
    /* Pointing at each other but not symmetrically paired. */
    reject("{\"a\": {\"object\": \"id\", \"inverse_of\": \"b\"},\n"
           " \"b\": {\"object\": \"id\", \"inverse_of\": \"c\"},\n"
           " \"c\": {\"object\": \"id\", \"inverse_of\": \"b\"}}",
           "not mutual");
    reject("{\"p\": {\"object\": \"id\", \"inverse_of\": \"p\"}}",
           "inverse_of itself");
    /* An inverse pair must both reference records. */
    reject("{\"p\": {\"object\": \"string\", \"inverse_of\": \"q\"},\n"
           " \"q\": {\"object\": \"string\", \"inverse_of\": \"p\"}}",
           "inverse pair");
    reject("{\"p\": {\"object\": \"id\", \"mutex_with\": [\"ghost\"]}}",
           "not declared");
    reject("{\"p\": {\"object\": \"id\", \"mutex_with\": [1]}}",
           "predicate names");
}

/* cJSON keeps duplicate keys, so without an explicit check one of them would
 * silently win — and which one is not something a file author can predict. */
static void test_rejects_a_duplicate_declaration(void) {
    reject("{\"p\": {\"object\": \"id\"}, \"p\": {\"object\": \"string\"}}",
           "declared twice");
}

/* A predicate the fact index could never intern would make every fact using it
 * fail at write time; naming it at startup is far better. */
static void test_rejects_an_overlong_predicate_name(void) {
    char json[256];
    char name[FACT_MAX_PREDICATE_LEN + 8];
    memset(name, 'p', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    snprintf(json, sizeof(json), "{\"%s\": {\"object\": \"id\"}}", name);
    reject(json, "longer than");
}

static void test_missing_file_is_a_clear_error(void) {
    char err[256] = "";
    TEST_ASSERT_NULL(predicate_registry_load("/nonexistent/aegis/registry.json",
                                             err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "cannot open"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_loads_a_valid_registry);
    RUN_TEST(test_check_enforces_membership_and_object_kind);
    RUN_TEST(test_null_registry_allows_everything);
    RUN_TEST(test_rejects_malformed_files);
    RUN_TEST(test_rejects_a_missing_or_bad_object_kind);
    RUN_TEST(test_rejects_unknown_keys);
    RUN_TEST(test_rejects_bad_scalars);
    RUN_TEST(test_rejects_relational_properties_on_a_literal);
    RUN_TEST(test_rejects_dangling_and_one_sided_references);
    RUN_TEST(test_rejects_a_duplicate_declaration);
    RUN_TEST(test_rejects_an_overlong_predicate_name);
    RUN_TEST(test_missing_file_is_a_clear_error);
    return UNITY_END();
}
