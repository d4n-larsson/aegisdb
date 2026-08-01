/* Unit tests for the lexical (BM25) index and its tokenizer — ROADMAP 4.1.
 *
 * The tokenizer tests are the important ones: the whole point of the feature is
 * that identifier-shaped terms survive indexing, so a tokenizer regression is a
 * silent recall regression. */
#include <stdlib.h>
#include <string.h>

#include "aegisdb/lexical_index.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- tokenizer ---------------------------------------------------------- */

/* Collect every term the tokenizer yields for `text` into `out` (owned strings);
 * returns the count. */
static size_t tokenize_all(const char *text, char out[][LEX_MAX_TERM + 1],
                           size_t max) {
    LexTokenizer t;
    lex_tokenizer_init(&t, text, strlen(text));
    char term[LEX_MAX_TERM + 1];
    size_t n = 0;
    while (n < max && lex_tokenizer_next(&t, term)) {
        memcpy(out[n], term, strlen(term) + 1);
        n++;
    }
    return n;
}

static int has_term(char terms[][LEX_MAX_TERM + 1], size_t n,
                    const char *want) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(terms[i], want) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_tokenizer_lowercases_and_splits_on_whitespace(void) {
    char terms[32][LEX_MAX_TERM + 1];
    size_t n = tokenize_all("Clean Rebuild Needed", terms, 32);
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_STRING("clean", terms[0]);
    TEST_ASSERT_EQUAL_STRING("rebuild", terms[1]);
    TEST_ASSERT_EQUAL_STRING("needed", terms[2]);
}

/* The core contract: a flag keeps its dashes, so it is findable by its exact
 * spelling — and also yields its words, so it is findable by one of them. */
static void test_tokenizer_keeps_flag_whole_and_emits_subparts(void) {
    char terms[32][LEX_MAX_TERM + 1];
    size_t n = tokenize_all("--tenant-max-records", terms, 32);
    TEST_ASSERT_EQUAL_STRING("tenant-max-records",
                             terms[0]); /* full term first */
    TEST_ASSERT_TRUE(has_term(terms, n, "tenant"));
    TEST_ASSERT_TRUE(has_term(terms, n, "max"));
    TEST_ASSERT_TRUE(has_term(terms, n, "records"));
}

/* A `file.c:line` reference survives intact; its one-character sub-part (`c`)
 * carries no signal and is dropped. */
static void test_tokenizer_keeps_file_line_reference(void) {
    char terms[32][LEX_MAX_TERM + 1];
    size_t n = tokenize_all("hnsw.c:214", terms, 32);
    TEST_ASSERT_EQUAL_STRING("hnsw.c:214", terms[0]);
    TEST_ASSERT_TRUE(has_term(terms, n, "hnsw"));
    TEST_ASSERT_TRUE(has_term(terms, n, "214"));
    TEST_ASSERT_FALSE(has_term(terms, n, "c"));
}

static void test_tokenizer_keeps_screaming_snake_case_whole(void) {
    char terms[32][LEX_MAX_TERM + 1];
    size_t n = tokenize_all("AEGIS_RECALL_TOP_K", terms, 32);
    TEST_ASSERT_EQUAL_STRING("aegis_recall_top_k", terms[0]);
    TEST_ASSERT_TRUE(has_term(terms, n, "aegis"));
    TEST_ASSERT_TRUE(has_term(terms, n, "recall"));
    TEST_ASSERT_TRUE(has_term(terms, n, "top"));
}

/* Sentence and bracket punctuation is trimmed from the edges, but the
 * identifier punctuation inside a term is not. */
static void test_tokenizer_trims_edge_punctuation(void) {
    char terms[32][LEX_MAX_TERM + 1];
    size_t n = tokenize_all("(coffee), tea. done:", terms, 32);
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_STRING("coffee", terms[0]);
    TEST_ASSERT_EQUAL_STRING("tea", terms[1]);
    TEST_ASSERT_EQUAL_STRING("done", terms[2]);
}

/* A leading underscore belongs to the identifier and must not be trimmed. */
static void test_tokenizer_keeps_leading_underscore(void) {
    char terms[32][LEX_MAX_TERM + 1];
    size_t n = tokenize_all("_private", terms, 32);
    TEST_ASSERT_EQUAL_STRING("_private", terms[0]);
    TEST_ASSERT_TRUE(has_term(terms, n, "private"));
}

static void test_tokenizer_truncates_overlong_term(void) {
    char long_term[LEX_MAX_TERM + 40];
    memset(long_term, 'a', sizeof(long_term) - 1);
    long_term[sizeof(long_term) - 1] = '\0';
    char terms[8][LEX_MAX_TERM + 1];
    size_t n = tokenize_all(long_term, terms, 8);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_size_t(LEX_MAX_TERM, strlen(terms[0]));
}

static void test_tokenizer_empty_and_punctuation_only(void) {
    char terms[8][LEX_MAX_TERM + 1];
    TEST_ASSERT_EQUAL_size_t(0, tokenize_all("", terms, 8));
    TEST_ASSERT_EQUAL_size_t(0, tokenize_all("   ", terms, 8));
    TEST_ASSERT_EQUAL_size_t(0, tokenize_all("!!! ??? ...", terms, 8));
}

/* Non-ASCII stays one opaque token rather than one token per UTF-8 byte. */
static void test_tokenizer_keeps_utf8_word_whole(void) {
    char terms[8][LEX_MAX_TERM + 1];
    size_t n = tokenize_all("naïve café", terms, 8);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_STRING("naïve", terms[0]);
    TEST_ASSERT_EQUAL_STRING("café", terms[1]);
}

/* ---- index + BM25 ------------------------------------------------------- */

static void add_doc(LexicalIndex *lx, uint64_t id, const char *text) {
    TEST_ASSERT_EQUAL_INT(0, lexical_index_add(lx, id, text, strlen(text)));
}

static void test_search_finds_exact_identifier(void) {
    LexicalIndex *lx = lexical_index_create();
    add_doc(lx, 1, "quotas are set with --tenant-max-records per namespace");
    add_doc(lx, 2, "the build needs a clean rebuild after a header edit");

    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, lexical_index_search(lx, "--tenant-max-records",
                                                  10, &ids, &scores, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(1, ids[0]);
    TEST_ASSERT_TRUE(scores[0] > 0.0F);
    free(ids);
    free(scores);
    lexical_index_free(lx);
}

/* A term in every document separates nothing; a rare term should outrank it. */
static void test_search_ranks_rare_term_above_common(void) {
    LexicalIndex *lx = lexical_index_create();
    add_doc(lx, 1, "the log the log the log rare_marker");
    add_doc(lx, 2, "the log the log the log the log");
    add_doc(lx, 3, "the log the log");

    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(
        0, lexical_index_search(lx, "log rare_marker", 10, &ids, &scores, &n));
    TEST_ASSERT_TRUE(n >= 1);
    TEST_ASSERT_EQUAL_UINT64(1, ids[0]); /* the only doc with the rare term */
    free(ids);
    free(scores);
    lexical_index_free(lx);
}

static void test_search_respects_top_k_and_orders_desc(void) {
    LexicalIndex *lx = lexical_index_create();
    for (uint64_t i = 1; i <= 10; i++) {
        add_doc(lx, i, "shared term here");
    }
    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(
        0, lexical_index_search(lx, "shared", 3, &ids, &scores, &n));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_TRUE(scores[0] >= scores[1]);
    TEST_ASSERT_TRUE(scores[1] >= scores[2]);
    /* Equal scores tie-break on ascending id, so paging is stable. */
    TEST_ASSERT_EQUAL_UINT64(1, ids[0]);
    TEST_ASSERT_EQUAL_UINT64(2, ids[1]);
    free(ids);
    free(scores);
    lexical_index_free(lx);
}

static void test_search_miss_and_empty_index(void) {
    LexicalIndex *lx = lexical_index_create();
    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t n = 0;

    /* Empty index. */
    TEST_ASSERT_EQUAL_INT(
        0, lexical_index_search(lx, "anything", 10, &ids, &scores, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_NULL(ids);

    add_doc(lx, 1, "present terms only");
    /* A term that is not indexed, and a query with no indexable terms at all. */
    TEST_ASSERT_EQUAL_INT(
        0, lexical_index_search(lx, "absent", 10, &ids, &scores, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_INT(
        0, lexical_index_search(lx, "!!!", 10, &ids, &scores, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    lexical_index_free(lx);
}

static void test_remove_unindexes_document(void) {
    LexicalIndex *lx = lexical_index_create();
    const char *text = "unique_token lives here";
    add_doc(lx, 7, text);
    TEST_ASSERT_EQUAL_size_t(1, lexical_index_docs(lx));

    lexical_index_remove(lx, 7, text, strlen(text));
    TEST_ASSERT_EQUAL_size_t(0, lexical_index_docs(lx));
    /* Every posting list the document contributed to is reclaimed with it. */
    TEST_ASSERT_EQUAL_size_t(0, lexical_index_terms(lx));

    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(
        0, lexical_index_search(lx, "unique_token", 10, &ids, &scores, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    lexical_index_free(lx);
}

/* Re-indexing an id already present must be refused, or the term frequencies
 * double and every later score is wrong. */
static void test_add_twice_does_not_double_count(void) {
    LexicalIndex *lx = lexical_index_create();
    const char *text = "alpha beta gamma";
    add_doc(lx, 1, text);
    size_t terms_after_first = lexical_index_terms(lx);

    TEST_ASSERT_EQUAL_INT(0, lexical_index_add(lx, 1, text, strlen(text)));
    TEST_ASSERT_EQUAL_size_t(terms_after_first, lexical_index_terms(lx));
    TEST_ASSERT_EQUAL_size_t(1, lexical_index_docs(lx));

    /* One remove is enough to fully unindex it. */
    lexical_index_remove(lx, 1, text, strlen(text));
    TEST_ASSERT_EQUAL_size_t(0, lexical_index_docs(lx));
    TEST_ASSERT_EQUAL_size_t(0, lexical_index_terms(lx));
    lexical_index_free(lx);
}

/* A document with nothing indexable is not a BM25 document at all. */
static void test_add_untokenizable_text_is_not_a_document(void) {
    LexicalIndex *lx = lexical_index_create();
    const char *text = "!!! ???";
    TEST_ASSERT_EQUAL_INT(0, lexical_index_add(lx, 1, text, strlen(text)));
    TEST_ASSERT_EQUAL_size_t(0, lexical_index_docs(lx));
    /* Removing it is a harmless no-op. */
    lexical_index_remove(lx, 1, text, strlen(text));
    TEST_ASSERT_EQUAL_size_t(0, lexical_index_docs(lx));
    lexical_index_free(lx);
}

static void test_null_and_empty_payloads(void) {
    LexicalIndex *lx = lexical_index_create();
    TEST_ASSERT_EQUAL_INT(0, lexical_index_add(lx, 1, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, lexical_index_add(lx, 2, "", 0));
    TEST_ASSERT_EQUAL_size_t(0, lexical_index_docs(lx));
    /* The NULL-index guards let call sites stay unconditional. */
    TEST_ASSERT_EQUAL_INT(0, lexical_index_add(NULL, 1, "x", 1));
    lexical_index_remove(NULL, 1, "x", 1);
    TEST_ASSERT_EQUAL_size_t(0, lexical_index_terms(NULL));
    TEST_ASSERT_EQUAL_size_t(0, lexical_index_docs(NULL));
    TEST_ASSERT_EQUAL_size_t(0, lexical_index_bytes(NULL));
    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(
        0, lexical_index_search(NULL, "x", 10, &ids, &scores, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    lexical_index_free(lx);
}

/* Churn: repeated add/remove of many documents must leave the index empty and
 * reclaimed, not slowly leak posting lists (the hazard tag_index_remove has). */
static void test_bulk_add_remove_reclaims(void) {
    LexicalIndex *lx = lexical_index_create();
    char text[64];
    for (uint64_t i = 1; i <= 500; i++) {
        snprintf(text, sizeof(text), "doc_%llu shared filler words",
                 (unsigned long long)i);
        add_doc(lx, i, text);
    }
    TEST_ASSERT_EQUAL_size_t(500, lexical_index_docs(lx));
    TEST_ASSERT_TRUE(lexical_index_bytes(lx) > 0);

    for (uint64_t i = 1; i <= 500; i++) {
        snprintf(text, sizeof(text), "doc_%llu shared filler words",
                 (unsigned long long)i);
        lexical_index_remove(lx, i, text, strlen(text));
    }
    TEST_ASSERT_EQUAL_size_t(0, lexical_index_docs(lx));
    TEST_ASSERT_EQUAL_size_t(0, lexical_index_terms(lx));
    lexical_index_free(lx);
}

/* Length normalisation: with the same single match, the shorter document wins. */
static void test_bm25_prefers_shorter_document(void) {
    LexicalIndex *lx = lexical_index_create();
    add_doc(lx, 1, "needle");
    add_doc(lx, 2,
            "needle surrounded by a great deal of unrelated filler text "
            "that dilutes the match considerably here");

    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(
        0, lexical_index_search(lx, "needle", 10, &ids, &scores, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT64(1, ids[0]);
    TEST_ASSERT_TRUE(scores[0] > scores[1]);
    free(ids);
    free(scores);
    lexical_index_free(lx);
}

/* A query longer than LEX_MAX_QUERY_TERMS must be bounded, not rejected. */
static void test_search_bounds_query_terms(void) {
    LexicalIndex *lx = lexical_index_create();
    add_doc(lx, 1, "alpha beta gamma delta");

    char big[LEX_MAX_QUERY_TERMS * 8 + 64];
    size_t off = 0;
    for (int i = 0; i < LEX_MAX_QUERY_TERMS + 20; i++) {
        off += (size_t)snprintf(big + off, sizeof(big) - off, "w%d ", i);
    }
    snprintf(big + off, sizeof(big) - off, "alpha");

    uint64_t *ids = NULL;
    float *scores = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0,
                          lexical_index_search(lx, big, 10, &ids, &scores, &n));
    /* `alpha` fell past the term cap, so the bound is what is asserted here —
     * the call is safe and bounded, not that this particular query matches. */
    free(ids);
    free(scores);
    lexical_index_free(lx);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_tokenizer_lowercases_and_splits_on_whitespace);
    RUN_TEST(test_tokenizer_keeps_flag_whole_and_emits_subparts);
    RUN_TEST(test_tokenizer_keeps_file_line_reference);
    RUN_TEST(test_tokenizer_keeps_screaming_snake_case_whole);
    RUN_TEST(test_tokenizer_trims_edge_punctuation);
    RUN_TEST(test_tokenizer_keeps_leading_underscore);
    RUN_TEST(test_tokenizer_truncates_overlong_term);
    RUN_TEST(test_tokenizer_empty_and_punctuation_only);
    RUN_TEST(test_tokenizer_keeps_utf8_word_whole);
    RUN_TEST(test_search_finds_exact_identifier);
    RUN_TEST(test_search_ranks_rare_term_above_common);
    RUN_TEST(test_search_respects_top_k_and_orders_desc);
    RUN_TEST(test_search_miss_and_empty_index);
    RUN_TEST(test_remove_unindexes_document);
    RUN_TEST(test_add_twice_does_not_double_count);
    RUN_TEST(test_add_untokenizable_text_is_not_a_document);
    RUN_TEST(test_null_and_empty_payloads);
    RUN_TEST(test_bulk_add_remove_reclaims);
    RUN_TEST(test_bm25_prefers_shorter_document);
    RUN_TEST(test_search_bounds_query_terms);
    return UNITY_END();
}