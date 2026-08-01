/* Inverted lexical index + Okapi BM25 ranking (ROADMAP 4.1). */
#include "aegisdb/lexical_index.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "aegisdb/hash_mix.h"

#define LEX_NBUCKETS 4096

/* Standard Okapi BM25 tuning: k1 damps term-frequency saturation, b controls how
 * strongly a long document is penalized. */
#define LEX_BM25_K1 1.2
#define LEX_BM25_B 0.75

/* --- character classes (the tokenizer contract lives in the header) ------- */

static int is_term_char(unsigned char c) {
    if (c >= 0x80) {
        return 1; /* UTF-8 lead/continuation byte: keep the word whole */
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
        return 1;
    }
    return c == '_' || c == '-' || c == '.' || c == ':' || c == '/' ||
           c == '+' || c == '#';
}

/* Punctuation allowed *inside* a term but never at its edge, so `foo.` indexes
 * as `foo` while `hnsw.c:214` stays whole. `_` is deliberately absent: a leading
 * underscore is part of the identifier, not decoration. */
static int is_edge_char(unsigned char c) {
    return c == '-' || c == '.' || c == ':' || c == '/' || c == '+' || c == '#';
}

/* Boundary between the sub-parts of a compound identifier. */
static int is_sub_delim(unsigned char c) { return c == '_' || is_edge_char(c); }

/* --- tokenizer ----------------------------------------------------------- */

void lex_tokenizer_init(LexTokenizer *t, const void *text, size_t len) {
    t->p = text ? (const unsigned char *)text : NULL;
    t->end = t->p ? t->p + len : NULL;
    t->full[0] = '\0';
    t->full_len = 0;
    t->sub_pos = 0;
    t->have_full = 0;
    t->compound = 0;
}

int lex_tokenizer_next(LexTokenizer *t, char out[LEX_MAX_TERM + 1]) {
    /* Sub-parts of the term just emitted: `--tenant-max-records` also yields
     * `tenant`, `max`, `records`, so a query for one word finds the flag. The
     * full term was emitted first and is never destroyed (ROADMAP 4.1). */
    while (t->have_full && t->compound && t->sub_pos < t->full_len) {
        size_t s = t->sub_pos;
        while (s < t->full_len && is_sub_delim((unsigned char)t->full[s])) {
            s++;
        }
        size_t e = s;
        while (e < t->full_len && !is_sub_delim((unsigned char)t->full[e])) {
            e++;
        }
        t->sub_pos = e;
        if (s >= t->full_len) {
            break; /* only delimiters left */
        }
        size_t n = e - s;
        /* A single character carries no signal (the `c` of `hnsw.c:214`), and a
         * sub-part spanning the whole term is just the term again. */
        if (n >= 2 && n < t->full_len) {
            memcpy(out, &t->full[s], n);
            out[n] = '\0';
            return 1;
        }
    }

    /* Next raw run of term characters: lowercased, edge-trimmed, length-capped. */
    t->have_full = 0;
    while (t->p < t->end) {
        while (t->p < t->end && !is_term_char(*t->p)) {
            t->p++;
        }
        const unsigned char *s = t->p;
        while (t->p < t->end && is_term_char(*t->p)) {
            t->p++;
        }
        const unsigned char *e = t->p;
        while (s < e && is_edge_char(*s)) {
            s++;
        }
        while (e > s && is_edge_char(e[-1])) {
            e--;
        }
        if (s == e) {
            continue; /* punctuation only */
        }
        size_t n = (size_t)(e - s);
        if (n > LEX_MAX_TERM) {
            n = LEX_MAX_TERM; /* truncate; query terms truncate identically */
        }
        int compound = 0;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = s[i];
            t->full[i] = (char)((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c);
            if (is_sub_delim(c)) {
                compound = 1;
            }
        }
        t->full[n] = '\0';
        t->full_len = n;
        t->compound = compound;
        t->sub_pos = 0;
        t->have_full = 1;
        memcpy(out, t->full, n + 1);
        return 1;
    }
    t->full_len = 0;
    return 0;
}

/* --- index structures ---------------------------------------------------- */

typedef struct {
    uint64_t id;
    uint32_t tf; /* occurrences of the term in this document */
} LexPost;

typedef struct LexTerm {
    char *term;
    LexPost *posts; /* sorted by id ascending */
    size_t n;
    size_t cap;
    struct LexTerm *next;
} LexTerm;

#define LEX_SLOT_EMPTY 0
#define LEX_SLOT_LIVE 1
#define LEX_SLOT_DEAD 2

/* Per-document length, which BM25 needs for its length norm (and whose count is
 * the corpus size N for idf). Open-addressed, linear probing. */
typedef struct {
    uint64_t id;
    uint32_t len; /* terms emitted for this document */
    uint8_t state;
} LexDoc;

struct LexicalIndex {
    LexTerm *buckets[LEX_NBUCKETS];
    size_t term_count;
    LexDoc *docs;
    size_t doc_cap;  /* power of two (0 until the first insert) */
    size_t doc_live; /* live documents = BM25 corpus size */
    size_t doc_used; /* live + dead slots, for the load factor */
    uint64_t total_len;
};

/* --- document table ------------------------------------------------------ */

/* The live slot for `id`, or NULL. Takes a const index but hands back a mutable
 * slot: the table is a member array, so its elements are not themselves const. */
static LexDoc *doc_slot(const LexicalIndex *lx, uint64_t id) {
    if (!lx->docs) {
        return NULL;
    }
    size_t mask = lx->doc_cap - 1;
    size_t i = (size_t)(mix64(id) & mask);
    for (size_t probe = 0; probe <= mask; probe++) {
        LexDoc *d = &lx->docs[(i + probe) & mask];
        if (d->state == LEX_SLOT_EMPTY) {
            return NULL;
        }
        if (d->state == LEX_SLOT_LIVE && d->id == id) {
            return d;
        }
    }
    return NULL;
}

static int doc_grow(LexicalIndex *lx) {
    size_t ncap = lx->doc_cap ? lx->doc_cap * 2 : 64;
    LexDoc *nd = calloc(ncap, sizeof(*nd));
    if (!nd) {
        return -1;
    }
    size_t mask = ncap - 1;
    for (size_t i = 0; i < lx->doc_cap; i++) {
        if (lx->docs[i].state != LEX_SLOT_LIVE) {
            continue; /* rehash drops the tombstones */
        }
        size_t j = (size_t)(mix64(lx->docs[i].id) & mask);
        while (nd[j].state == LEX_SLOT_LIVE) {
            j = (j + 1) & mask;
        }
        nd[j] = lx->docs[i];
    }
    free(lx->docs);
    lx->docs = nd;
    lx->doc_cap = ncap;
    lx->doc_used = lx->doc_live;
    return 0;
}

static int doc_put(LexicalIndex *lx, uint64_t id, uint32_t len) {
    if (!lx->doc_cap || ((lx->doc_used + 1) * 4) >= (lx->doc_cap * 3)) {
        if (doc_grow(lx) != 0) {
            return -1;
        }
    }
    size_t mask = lx->doc_cap - 1;
    size_t i = (size_t)(mix64(id) & mask);
    LexDoc *reuse = NULL;
    for (size_t probe = 0; probe <= mask; probe++) {
        LexDoc *d = &lx->docs[(i + probe) & mask];
        if (d->state == LEX_SLOT_LIVE && d->id == id) {
            return 0; /* already present (callers check first) */
        }
        if (d->state == LEX_SLOT_DEAD && !reuse) {
            reuse = d;
        }
        if (d->state == LEX_SLOT_EMPTY) {
            LexDoc *slot = reuse ? reuse : d;
            if (!reuse) {
                lx->doc_used++;
            }
            slot->id = id;
            slot->len = len;
            slot->state = LEX_SLOT_LIVE;
            lx->doc_live++;
            lx->total_len += len;
            return 0;
        }
    }
    return -1; /* table full: cannot happen at a 0.75 load factor */
}

static void doc_del(LexicalIndex *lx, uint64_t id) {
    LexDoc *d = doc_slot(lx, id);
    if (!d) {
        return;
    }
    lx->total_len -= d->len;
    d->state = LEX_SLOT_DEAD;
    lx->doc_live--;
}

/* --- term table ---------------------------------------------------------- */

static size_t term_bucket(const char *s) {
    size_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h % LEX_NBUCKETS;
}

static LexTerm *term_find(const LexicalIndex *lx, const char *term) {
    for (LexTerm *n = lx->buckets[term_bucket(term)]; n; n = n->next) {
        if (strcmp(n->term, term) == 0) {
            return n;
        }
    }
    return NULL;
}

/* Index of the first posting with id >= `id`. */
static size_t post_lower_bound(const LexPost *p, size_t n, uint64_t id) {
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) / 2);
        if (p[mid].id < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* Record one occurrence of `term` in `id`. */
static int term_add(LexicalIndex *lx, const char *term, uint64_t id) {
    size_t b = term_bucket(term);
    LexTerm *n = term_find(lx, term);
    if (!n) {
        n = calloc(1, sizeof(*n));
        if (!n) {
            return -1;
        }
        n->term = strdup(term);
        if (!n->term) {
            free(n);
            return -1;
        }
        n->next = lx->buckets[b];
        lx->buckets[b] = n;
        lx->term_count++;
    }
    size_t pos = post_lower_bound(n->posts, n->n, id);
    if (pos < n->n && n->posts[pos].id == id) {
        if (n->posts[pos].tf < UINT32_MAX) {
            n->posts[pos].tf++;
        }
        return 0;
    }
    if (n->n == n->cap) {
        size_t cap = n->cap ? n->cap * 2 : 4;
        LexPost *np = realloc(n->posts, cap * sizeof(*np));
        if (!np) {
            return -1;
        }
        n->posts = np;
        n->cap = cap;
    }
    if (pos < n->n) {
        memmove(&n->posts[pos + 1], &n->posts[pos],
                (n->n - pos) * sizeof(LexPost));
    }
    n->posts[pos].id = id;
    n->posts[pos].tf = 1;
    n->n++;
    return 0;
}

/* Drop `id` from `term` entirely (the whole document is going away, so the term
 * frequency is irrelevant). Reclaims a term that no longer occurs anywhere. */
static void term_remove(LexicalIndex *lx, const char *term, uint64_t id) {
    size_t b = term_bucket(term);
    LexTerm *prev = NULL;
    for (LexTerm *n = lx->buckets[b]; n; prev = n, n = n->next) {
        if (strcmp(n->term, term) != 0) {
            continue;
        }
        size_t pos = post_lower_bound(n->posts, n->n, id);
        if (pos < n->n && n->posts[pos].id == id) {
            memmove(&n->posts[pos], &n->posts[pos + 1],
                    (n->n - pos - 1) * sizeof(LexPost));
            n->n--;
        }
        /* Reclaim the empty node, or repeated add/remove of distinct terms would
         * grow the bucket chains without bound (as in tag_index_remove). */
        if (n->n == 0) {
            if (prev) {
                prev->next = n->next;
            } else {
                lx->buckets[b] = n->next;
            }
            free(n->posts);
            free(n->term);
            free(n);
            lx->term_count--;
        }
        return;
    }
}

/* --- lifecycle ----------------------------------------------------------- */

LexicalIndex *lexical_index_create(void) {
    return calloc(1, sizeof(LexicalIndex));
}

void lexical_index_free(LexicalIndex *lx) {
    if (!lx) {
        return;
    }
    for (size_t i = 0; i < LEX_NBUCKETS; i++) {
        LexTerm *n = lx->buckets[i];
        while (n) {
            LexTerm *nx = n->next;
            free(n->posts);
            free(n->term);
            free(n);
            n = nx;
        }
    }
    free(lx->docs);
    free(lx);
}

size_t lexical_index_terms(const LexicalIndex *lx) {
    return lx ? lx->term_count : 0;
}

size_t lexical_index_docs(const LexicalIndex *lx) {
    return lx ? lx->doc_live : 0;
}

size_t lexical_index_bytes(const LexicalIndex *lx) {
    if (!lx) {
        return 0;
    }
    size_t total = sizeof(*lx) + (lx->doc_cap * sizeof(LexDoc));
    for (size_t i = 0; i < LEX_NBUCKETS; i++) {
        for (const LexTerm *n = lx->buckets[i]; n; n = n->next) {
            total += sizeof(*n) + (n->term ? strlen(n->term) + 1 : 0) +
                     (n->cap * sizeof(LexPost));
        }
    }
    return total;
}

/* --- write path ---------------------------------------------------------- */

/* Unindex every term the text tokenizes to. Used both by the public remove and
 * to roll back a partially applied add. */
static void unindex_text(LexicalIndex *lx, uint64_t id, const void *text,
                         size_t len) {
    LexTokenizer t;
    lex_tokenizer_init(&t, text, len);
    char term[LEX_MAX_TERM + 1];
    while (lex_tokenizer_next(&t, term)) {
        term_remove(lx, term, id);
    }
}

int lexical_index_add(LexicalIndex *lx, uint64_t id, const void *text,
                      size_t len) {
    if (!lx) {
        return 0; /* --no-lexical-index: nothing to do, not a failure */
    }
    if (doc_slot(lx, id)) {
        return 0; /* already indexed — see the header: never double-count */
    }
    LexTokenizer t;
    lex_tokenizer_init(&t, text, len);
    char term[LEX_MAX_TERM + 1];
    uint32_t dl = 0;
    while (lex_tokenizer_next(&t, term)) {
        if (term_add(lx, term, id) != 0) {
            /* Roll back: a half-indexed document would skew every later score. */
            unindex_text(lx, id, text, len);
            return -1;
        }
        if (dl < UINT32_MAX) {
            dl++;
        }
    }
    if (dl == 0) {
        return 0; /* nothing indexable: not a document as far as BM25 cares */
    }
    if (doc_put(lx, id, dl) != 0) {
        unindex_text(lx, id, text, len);
        return -1;
    }
    return 0;
}

void lexical_index_remove(LexicalIndex *lx, uint64_t id, const void *text,
                          size_t len) {
    if (!lx || !doc_slot(lx, id)) {
        return;
    }
    unindex_text(lx, id, text, len);
    doc_del(lx, id);
}

/* --- search -------------------------------------------------------------- */

typedef struct {
    uint64_t id;
    float score;
} LexCand;

/* id -> index into the candidate array. Open-addressed; rebuilt from the
 * candidates on growth, so the stored indices stay valid across a realloc. */
typedef struct {
    uint64_t id;
    uint32_t idx;
    uint8_t used;
} LexAcc;

/* Either the slot holding `id` or the empty slot it belongs in. The load factor
 * is kept below 1, so the probe always terminates. */
static size_t acc_find(const LexAcc *acc, size_t acap, uint64_t id) {
    size_t mask = acap - 1;
    size_t i = (size_t)(mix64(id) & mask);
    while (acc[i].used && acc[i].id != id) {
        i = (i + 1) & mask;
    }
    return i;
}

static int acc_grow(LexAcc **acc, size_t *acap, const LexCand *cands,
                    size_t cn) {
    size_t ncap = *acap * 2;
    LexAcc *na = calloc(ncap, sizeof(*na));
    if (!na) {
        return -1;
    }
    for (size_t i = 0; i < cn; i++) {
        size_t s = acc_find(na, ncap, cands[i].id);
        na[s].used = 1;
        na[s].id = cands[i].id;
        na[s].idx = (uint32_t)i;
    }
    free(*acc);
    *acc = na;
    *acap = ncap;
    return 0;
}

static int cmp_cand_desc(const void *a, const void *b) {
    const LexCand *x = a;
    const LexCand *y = b;
    if (x->score < y->score) {
        return 1;
    }
    if (x->score > y->score) {
        return -1;
    }
    /* Ties break on ascending id so paging over equal scores is stable. */
    if (x->id < y->id) {
        return -1;
    }
    if (x->id > y->id) {
        return 1;
    }
    return 0;
}

int lexical_index_search(const LexicalIndex *lx, const char *query,
                         size_t top_k, uint64_t **out_ids, float **out_scores,
                         size_t *out_n) {
    *out_ids = NULL;
    *out_scores = NULL;
    *out_n = 0;
    if (!lx || !query || !top_k || lx->doc_live == 0) {
        return 0;
    }

    /* Tokenize the query the same way documents were, deduping: a word repeated
     * in the query must not weight it twice. */
    char terms[LEX_MAX_QUERY_TERMS][LEX_MAX_TERM + 1];
    size_t tn = 0;
    LexTokenizer tk;
    lex_tokenizer_init(&tk, query, strlen(query));
    char term[LEX_MAX_TERM + 1];
    while (tn < LEX_MAX_QUERY_TERMS && lex_tokenizer_next(&tk, term)) {
        int dup = 0;
        for (size_t i = 0; i < tn; i++) {
            if (strcmp(terms[i], term) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            memcpy(terms[tn], term, strlen(term) + 1);
            tn++;
        }
    }

    /* Resolve to posting lists, rarest first: with a bounded candidate set the
     * most informative terms should be the ones that populate it. */
    const LexTerm *nodes[LEX_MAX_QUERY_TERMS];
    size_t nn = 0;
    for (size_t i = 0; i < tn; i++) {
        const LexTerm *node = term_find(lx, terms[i]);
        if (node && node->n) {
            nodes[nn++] = node;
        }
    }
    if (nn == 0) {
        return 0;
    }
    for (size_t i = 1; i < nn; i++) { /* insertion sort; nn <= 32 */
        const LexTerm *v = nodes[i];
        size_t j = i;
        while (j > 0 && nodes[j - 1]->n > v->n) {
            nodes[j] = nodes[j - 1];
            j--;
        }
        nodes[j] = v;
    }

    double n_docs = (double)lx->doc_live;
    double avgdl = lx->total_len ? ((double)lx->total_len / n_docs) : 1.0;
    if (avgdl <= 0.0) {
        avgdl = 1.0;
    }

    size_t acap = 256;
    LexAcc *acc = calloc(acap, sizeof(*acc));
    size_t ccap = 64;
    LexCand *cands = malloc(ccap * sizeof(*cands));
    if (!acc || !cands) {
        free(acc);
        free(cands);
        return -1;
    }
    size_t cn = 0;

    for (size_t t = 0; t < nn; t++) {
        const LexTerm *node = nodes[t];
        double df = (double)node->n;
        double idf = log(1.0 + ((n_docs - df + 0.5) / (df + 0.5)));
        if (idf <= 0.0) {
            continue; /* a term in every document separates nothing */
        }
        for (size_t i = 0; i < node->n; i++) {
            uint64_t id = node->posts[i].id;
            const LexDoc *d = doc_slot(lx, id);
            if (!d) {
                continue; /* defensive: posting outlived its document */
            }
            double tf = (double)node->posts[i].tf;
            double norm =
                tf + (LEX_BM25_K1 * (1.0 - LEX_BM25_B +
                                     (LEX_BM25_B * (double)d->len / avgdl)));
            double contrib =
                idf * ((tf * (LEX_BM25_K1 + 1.0)) / (norm > 0.0 ? norm : 1.0));

            size_t s = acc_find(acc, acap, id);
            if (acc[s].used) {
                cands[acc[s].idx].score += (float)contrib;
                continue;
            }
            if (cn >= LEX_MAX_CANDIDATES) {
                continue; /* bounded work; see the header */
            }
            if (cn == ccap) {
                size_t nc = ccap * 2;
                LexCand *ncands = realloc(cands, nc * sizeof(*ncands));
                if (!ncands) {
                    free(acc);
                    free(cands);
                    return -1;
                }
                cands = ncands;
                ccap = nc;
            }
            cands[cn].id = id;
            cands[cn].score = (float)contrib;
            acc[s].used = 1;
            acc[s].id = id;
            acc[s].idx = (uint32_t)cn;
            cn++;
            /* Grow after inserting, so the slot resolved above stays valid. */
            if (((cn + 1) * 4) >= (acap * 3)) {
                if (acc_grow(&acc, &acap, cands, cn) != 0) {
                    free(acc);
                    free(cands);
                    return -1;
                }
            }
        }
    }
    free(acc);

    if (cn == 0) {
        free(cands);
        return 0;
    }
    qsort(cands, cn, sizeof(*cands), cmp_cand_desc);
    size_t n = cn < top_k ? cn : top_k;
    uint64_t *ids = malloc(n * sizeof(*ids));
    float *scores = malloc(n * sizeof(*scores));
    if (!ids || !scores) {
        free(ids);
        free(scores);
        free(cands);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        ids[i] = cands[i].id;
        scores[i] = cands[i].score;
    }
    free(cands);
    *out_ids = ids;
    *out_scores = scores;
    *out_n = n;
    return 0;
}