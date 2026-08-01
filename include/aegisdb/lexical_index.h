/* Inverted lexical index (ROADMAP 4.1): term -> postings, plus the per-document
 * statistics BM25 needs.
 *
 * Dense embeddings average rare tokens away, so a memory holding
 * `--tenant-max-records` or `hnsw.c:214` is effectively unfindable by those
 * exact terms — and a server configured without an embedding provider has no
 * content-based retrieval path at all. This index supplies both.
 *
 * It is in-RAM and *derived*: nothing is persisted, and recovery rebuilds it
 * from the log exactly as it rebuilds the tag and time indexes. Not internally
 * synchronized — callers hold db->index_lock, as they do for the tag index.
 */
#ifndef AEGISDB_LEXICAL_INDEX_H
#define AEGISDB_LEXICAL_INDEX_H

#include <stddef.h>
#include <stdint.h>

/* Longest indexed term. A longer run is truncated rather than dropped (which
 * would make a very long identifier unfindable); query terms are truncated the
 * same way, so the two still match. */
#define LEX_MAX_TERM 64

/* Terms taken from one query, after dedup. Bounds the per-query posting walk. */
#define LEX_MAX_QUERY_TERMS 32

/* Documents one query may accumulate a score for. A common term's posting list
 * is as long as the corpus, so this is what keeps an untrusted query's work (and
 * allocation) bounded rather than proportional to the dataset. */
#define LEX_MAX_CANDIDATES 65536

typedef struct LexicalIndex LexicalIndex;

LexicalIndex *lexical_index_create(void);
void lexical_index_free(LexicalIndex *lx);

/* Index `len` bytes of `text` under `id`. If `id` is already indexed this is a
 * no-op returning 0: callers unindex the old text first, and a silent second
 * index would double the term frequencies and corrupt every later score.
 * Returns 0 on success (including the no-op), -1 on allocation failure.
 *
 * Every entry point tolerates a NULL index and treats it as "no lexical index
 * configured" (--no-lexical-index), so the write path can call these
 * unconditionally rather than guarding each site. */
int lexical_index_add(LexicalIndex *lx, uint64_t id, const void *text,
                      size_t len);

/* Unindex `id`. `text`/`len` must be the bytes that were indexed for it — every
 * caller holds the record version it is replacing — which keeps removal
 * O(tokens) instead of a walk over every posting list in the index. */
void lexical_index_remove(LexicalIndex *lx, uint64_t id, const void *text,
                          size_t len);

/* Rank documents against `query` by Okapi BM25, best-first. Allocates *out_ids
 * and *out_scores (two parallel arrays of *out_n, each freed with free()) and
 * returns at most `top_k`. A query with no indexable terms, or an empty index,
 * yields *out_n == 0 (with NULL arrays) and still returns 0. Ties break on
 * ascending id so paging is stable. Returns 0/-1.
 *
 * Bounded work: at most LEX_MAX_QUERY_TERMS terms are scored, rarest term
 * first, and the candidate set stops growing at LEX_MAX_CANDIDATES — so a
 * stopword-heavy query costs a bounded walk rather than one proportional to the
 * whole corpus. Past that bound the result is the best of the candidates seen,
 * not a guaranteed global top-k. */
int lexical_index_search(const LexicalIndex *lx, const char *query,
                         size_t top_k, uint64_t **out_ids, float **out_scores,
                         size_t *out_n);

/* Distinct terms indexed. */
size_t lexical_index_terms(const LexicalIndex *lx);
/* Documents indexed (the BM25 corpus size). */
size_t lexical_index_docs(const LexicalIndex *lx);
/* Approximate resident bytes (bucket table + term nodes + postings + doc
 * table). Excludes allocator overhead. */
size_t lexical_index_bytes(const LexicalIndex *lx);

/* ----- tokenizer ---------------------------------------------------------
 *
 * One pass over text yielding lowercased terms. Identifier shape is preserved:
 * `_ - . : / + #` and any byte >= 0x80 stay *inside* a term, so
 * `--tenant-max-records` and `hnsw.c:214` survive as single terms instead of
 * being shredded into unsearchable fragments. Edge punctuation is trimmed
 * (`foo.` -> `foo`), and a compound term additionally yields its alphanumeric
 * sub-parts of two or more characters (`tenant`, `max`, `records`) so a query
 * for one word still finds the flag. The full term is always emitted first.
 *
 * Exposed so both the index and the query path tokenize identically, and so the
 * rules above are directly testable.
 */
typedef struct {
    const unsigned char *p;
    const unsigned char *end;
    char full[LEX_MAX_TERM + 1]; /* the current full term */
    size_t full_len;
    size_t sub_pos; /* cursor into full[] while emitting sub-parts */
    int have_full;  /* a full term is loaded and has been emitted */
    int compound;   /* full[] holds a delimiter, so sub-parts exist */
} LexTokenizer;

void lex_tokenizer_init(LexTokenizer *t, const void *text, size_t len);

/* Write the next term into `out` (NUL-terminated). Returns 1 on a term, 0 when
 * the text is exhausted. */
int lex_tokenizer_next(LexTokenizer *t, char out[LEX_MAX_TERM + 1]);

#endif /* AEGISDB_LEXICAL_INDEX_H */