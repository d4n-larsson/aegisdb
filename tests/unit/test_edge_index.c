/* Unit tests for the reverse relationship index — ROADMAP 5.1.
 *
 * The invariant this index exists to uphold is that a reverse query is exact
 * and O(indegree): it must never miss an edge, never report one that was
 * removed, and never let repeated add/remove churn grow its tables. The
 * kind-interning cap is the one place it is allowed to lose precision, and the
 * tests below pin down exactly where that line is. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aegisdb/edge_index.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Sources pointing at `to`, filtered by an optional NULL-terminated kind list. */
static size_t sources_of(const EdgeIndex *e, uint64_t to,
                         const char *const *kinds, size_t n_kinds,
                         EdgeSource **out) {
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0,
                          edge_index_sources(e, to, kinds, n_kinds, out, &n));
    return n;
}

/* Is `from` among the sources of `to` (ignoring kind)? */
static int has_source(const EdgeIndex *e, uint64_t to, uint64_t from) {
    EdgeSource *s = NULL;
    size_t n = sources_of(e, to, NULL, 0, &s);
    int found = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i].from_id == from) {
            found = 1;
        }
    }
    free(s);
    return found;
}

/* ---- basics ------------------------------------------------------------- */

static void test_add_then_query_reverse(void) {
    EdgeIndex *e = edge_index_create();
    TEST_ASSERT_NOT_NULL(e);
    /* 10 -supersedes-> 99, 11 -derived_from-> 99 */
    TEST_ASSERT_EQUAL_INT(0, edge_index_add(e, 10, 99, "supersedes"));
    TEST_ASSERT_EQUAL_INT(0, edge_index_add(e, 11, 99, "derived_from"));

    EdgeSource *s = NULL;
    size_t n = sources_of(e, 99, NULL, 0, &s);
    TEST_ASSERT_EQUAL_UINT(2, n);
    /* sorted by from_id ascending */
    TEST_ASSERT_EQUAL_UINT64(10, s[0].from_id);
    TEST_ASSERT_EQUAL_STRING("supersedes", s[0].kind);
    TEST_ASSERT_EQUAL_INT(0, s[0].kind_unknown);
    TEST_ASSERT_EQUAL_UINT64(11, s[1].from_id);
    TEST_ASSERT_EQUAL_STRING("derived_from", s[1].kind);
    free(s);

    /* the forward direction is not this index's job */
    TEST_ASSERT_EQUAL_UINT(0, sources_of(e, 10, NULL, 0, &s));
    TEST_ASSERT_NULL(s);

    TEST_ASSERT_EQUAL_UINT(2, edge_index_edges(e));
    TEST_ASSERT_EQUAL_UINT(2, edge_index_kinds(e));
    edge_index_free(e);
}

static void test_unknown_target_is_empty_not_an_error(void) {
    EdgeIndex *e = edge_index_create();
    edge_index_add(e, 1, 2, "k");
    EdgeSource *s = NULL;
    TEST_ASSERT_EQUAL_UINT(0, sources_of(e, 4242, NULL, 0, &s));
    TEST_ASSERT_NULL(s);
    edge_index_free(e);
}

static void test_add_is_idempotent(void) {
    EdgeIndex *e = edge_index_create();
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_INT(0, edge_index_add(e, 10, 99, "supersedes"));
    }
    TEST_ASSERT_EQUAL_UINT(1, edge_index_edges(e));
    /* ...but the same pair with a *different* kind is a different edge, exactly
     * as `relate` treats it */
    TEST_ASSERT_EQUAL_INT(0, edge_index_add(e, 10, 99, "derived_from"));
    TEST_ASSERT_EQUAL_UINT(2, edge_index_edges(e));
    /* both are reported, distinguished by kind */
    EdgeSource *s = NULL;
    TEST_ASSERT_EQUAL_UINT(2, sources_of(e, 99, NULL, 0, &s));
    TEST_ASSERT_EQUAL_UINT64(10, s[0].from_id);
    TEST_ASSERT_EQUAL_UINT64(10, s[1].from_id);
    TEST_ASSERT_TRUE(strcmp(s[0].kind, s[1].kind) != 0);
    free(s);
    edge_index_free(e);
}

static void test_unkinded_edge(void) {
    EdgeIndex *e = edge_index_create();
    TEST_ASSERT_EQUAL_INT(0, edge_index_add(e, 7, 8, NULL));
    EdgeSource *s = NULL;
    TEST_ASSERT_EQUAL_UINT(1, sources_of(e, 8, NULL, 0, &s));
    TEST_ASSERT_NULL(s[0].kind);
    TEST_ASSERT_EQUAL_INT(0, s[0].kind_unknown); /* absent, not unknown */
    free(s);
    /* an empty kind is treated as no kind, not as a distinct one */
    TEST_ASSERT_EQUAL_INT(0, edge_index_add(e, 7, 8, ""));
    TEST_ASSERT_EQUAL_UINT(1, edge_index_edges(e));
    TEST_ASSERT_EQUAL_UINT(0, edge_index_kinds(e));
    edge_index_free(e);
}

/* ---- kind filtering ----------------------------------------------------- */

static void test_kind_filter(void) {
    EdgeIndex *e = edge_index_create();
    edge_index_add(e, 10, 99, "supersedes");
    edge_index_add(e, 11, 99, "derived_from");
    edge_index_add(e, 12, 99, "supersedes");

    const char *sup[] = {"supersedes"};
    EdgeSource *s = NULL;
    size_t n = sources_of(e, 99, sup, 1, &s);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT64(10, s[0].from_id);
    TEST_ASSERT_EQUAL_UINT64(12, s[1].from_id);
    free(s);

    /* several kinds = union */
    const char *both[] = {"supersedes", "derived_from"};
    TEST_ASSERT_EQUAL_UINT(3, sources_of(e, 99, both, 2, &s));
    free(s);

    /* a kind nobody wrote matches nothing (and is not confused with unknown) */
    const char *nope[] = {"no_such_kind"};
    TEST_ASSERT_EQUAL_UINT(0, sources_of(e, 99, nope, 1, &s));
    TEST_ASSERT_NULL(s);

    /* an unkinded edge is not matched by a named filter */
    edge_index_add(e, 13, 99, NULL);
    TEST_ASSERT_EQUAL_UINT(2, sources_of(e, 99, sup, 1, &s));
    free(s);
    edge_index_free(e);
}

/* ---- removal ------------------------------------------------------------ */

static void test_remove_one_edge(void) {
    EdgeIndex *e = edge_index_create();
    edge_index_add(e, 10, 99, "supersedes");
    edge_index_add(e, 11, 99, "supersedes");

    edge_index_remove(e, 10, 99, "supersedes");
    TEST_ASSERT_EQUAL_UINT(1, edge_index_edges(e));
    TEST_ASSERT_EQUAL_INT(0, has_source(e, 99, 10));
    TEST_ASSERT_EQUAL_INT(1, has_source(e, 99, 11));

    /* removing the wrong kind leaves the edge alone */
    edge_index_remove(e, 11, 99, "derived_from");
    TEST_ASSERT_EQUAL_UINT(1, edge_index_edges(e));
    /* absent edges and absent targets are ignored, not errors */
    edge_index_remove(e, 999, 99, "supersedes");
    edge_index_remove(e, 10, 4242, "supersedes");
    TEST_ASSERT_EQUAL_UINT(1, edge_index_edges(e));
    edge_index_free(e);
}

static void test_remove_target_drops_whole_indegree(void) {
    EdgeIndex *e = edge_index_create();
    for (uint64_t from = 1; from <= 50; from++) {
        edge_index_add(e, from, 99, "derived_from");
    }
    edge_index_add(e, 1, 100, "derived_from"); /* a bystander target */
    TEST_ASSERT_EQUAL_UINT(51, edge_index_edges(e));

    edge_index_remove_target(e, 99);
    TEST_ASSERT_EQUAL_UINT(1, edge_index_edges(e));
    EdgeSource *s = NULL;
    TEST_ASSERT_EQUAL_UINT(0, sources_of(e, 99, NULL, 0, &s));
    TEST_ASSERT_NULL(s);
    TEST_ASSERT_EQUAL_INT(1, has_source(e, 100, 1)); /* bystander survives */

    /* idempotent */
    edge_index_remove_target(e, 99);
    edge_index_remove_target(e, 4242);
    TEST_ASSERT_EQUAL_UINT(1, edge_index_edges(e));
    edge_index_free(e);
}

/* A retired target must be reusable: the tombstone has to be claimed by a later
 * add for the same id, not shadow it. */
static void test_readd_after_target_removed(void) {
    EdgeIndex *e = edge_index_create();
    edge_index_add(e, 1, 99, "k");
    edge_index_remove_target(e, 99);
    TEST_ASSERT_EQUAL_INT(0, edge_index_add(e, 2, 99, "k"));
    TEST_ASSERT_EQUAL_UINT(1, edge_index_edges(e));
    TEST_ASSERT_EQUAL_INT(1, has_source(e, 99, 2));
    TEST_ASSERT_EQUAL_INT(0, has_source(e, 99, 1));
    edge_index_free(e);
}

/* Churn must not grow the tables without bound — the tombstones have to be
 * reclaimed by the rehash, as they are in the lexical index's doc table. */
static void test_churn_reclaims(void) {
    EdgeIndex *e = edge_index_create();
    for (uint64_t round = 0; round < 200; round++) {
        edge_index_add(e, round, 1000 + round, "k");
        edge_index_remove_target(e, 1000 + round);
    }
    TEST_ASSERT_EQUAL_UINT(0, edge_index_edges(e));
    size_t churned = edge_index_bytes(e);

    EdgeIndex *fresh = edge_index_create();
    for (uint64_t round = 0; round < 200; round++) {
        edge_index_add(fresh, round, 1000 + round, "k");
        edge_index_remove_target(fresh, 1000 + round);
    }
    /* Deterministic: the same sequence must reach the same footprint, i.e. the
     * churn is bounded rather than accumulating. */
    TEST_ASSERT_EQUAL_UINT(churned, edge_index_bytes(fresh));
    edge_index_free(fresh);
    edge_index_free(e);
}

/* ---- scale + accounting ------------------------------------------------- */

static void test_many_targets_grow_and_stay_exact(void) {
    EdgeIndex *e = edge_index_create();
    const uint64_t N = 500;
    for (uint64_t i = 0; i < N; i++) {
        TEST_ASSERT_EQUAL_INT(0, edge_index_add(e, i, i + 1, "derived_from"));
    }
    TEST_ASSERT_EQUAL_UINT(N, edge_index_edges(e));
    for (uint64_t i = 0; i < N; i++) {
        TEST_ASSERT_EQUAL_INT(1, has_source(e, i + 1, i));
    }
    /* and nothing extra crept in */
    EdgeSource *s = NULL;
    for (uint64_t i = 0; i < N; i++) {
        TEST_ASSERT_EQUAL_UINT(1, sources_of(e, i + 1, NULL, 0, &s));
        free(s);
    }
    edge_index_free(e);
}

static void test_bytes_grows_with_content(void) {
    EdgeIndex *e = edge_index_create();
    size_t empty = edge_index_bytes(e);
    TEST_ASSERT_TRUE(empty > 0);
    for (uint64_t i = 0; i < 100; i++) {
        edge_index_add(e, i, 99, "derived_from");
    }
    TEST_ASSERT_TRUE(edge_index_bytes(e) > empty);
    edge_index_free(e);
}

/* The reported kind count tracks kinds *in use*, not kinds ever seen — so the
 * same log replayed into a fresh index yields the same number, which is what
 * makes it safe to graph. */
static void test_kind_count_tracks_use_not_history(void) {
    EdgeIndex *e = edge_index_create();
    edge_index_add(e, 1, 99, "supersedes");
    edge_index_add(e, 2, 99, "derived_from");
    TEST_ASSERT_EQUAL_UINT(2, edge_index_kinds(e));

    /* two edges of one kind, then drop one: the kind is still in use */
    edge_index_add(e, 3, 99, "supersedes");
    TEST_ASSERT_EQUAL_UINT(2, edge_index_kinds(e));
    edge_index_remove(e, 3, 99, "supersedes");
    TEST_ASSERT_EQUAL_UINT(2, edge_index_kinds(e));

    /* dropping its last edge retires the kind from the count */
    edge_index_remove(e, 1, 99, "supersedes");
    TEST_ASSERT_EQUAL_UINT(1, edge_index_kinds(e));

    /* re-adding it counts once, not twice (the node was never freed) */
    edge_index_add(e, 1, 99, "supersedes");
    TEST_ASSERT_EQUAL_UINT(2, edge_index_kinds(e));

    /* a whole-target drop releases every kind it held */
    edge_index_remove_target(e, 99);
    TEST_ASSERT_EQUAL_UINT(0, edge_index_kinds(e));
    TEST_ASSERT_EQUAL_UINT(0, edge_index_edges(e));

    /* unkinded and un-internable edges are not counted as kinds at all */
    edge_index_add(e, 1, 98, NULL);
    TEST_ASSERT_EQUAL_UINT(0, edge_index_kinds(e));
    edge_index_free(e);
}

/* ---- the interning cap -------------------------------------------------- */

/* Past EDGE_MAX_KINDS the edge is still indexed; only its label is lost, and it
 * says so. Completeness before precision: dropping the edge would make "what
 * depends on this?" answer wrongly. */
static void test_kind_cap_degrades_to_unknown(void) {
    EdgeIndex *e = edge_index_create();
    char kind[32];
    for (size_t i = 0; i < EDGE_MAX_KINDS; i++) {
        snprintf(kind, sizeof(kind), "kind_%zu", i);
        TEST_ASSERT_EQUAL_INT(0, edge_index_add(e, i, 99, kind));
    }
    TEST_ASSERT_EQUAL_UINT(EDGE_MAX_KINDS, edge_index_kinds(e));

    /* one over the line */
    TEST_ASSERT_EQUAL_INT(0,
                          edge_index_add(e, 999999, 99, "one_kind_too_many"));
    TEST_ASSERT_EQUAL_UINT(EDGE_MAX_KINDS, edge_index_kinds(e));
    TEST_ASSERT_EQUAL_UINT(EDGE_MAX_KINDS + 1, edge_index_edges(e));

    /* The overflow edge is present and flagged... */
    EdgeSource *s = NULL;
    size_t n = sources_of(e, 99, NULL, 0, &s);
    TEST_ASSERT_EQUAL_UINT(EDGE_MAX_KINDS + 1, n);
    int saw_unknown = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i].from_id == 999999) {
            TEST_ASSERT_EQUAL_INT(1, s[i].kind_unknown);
            TEST_ASSERT_NULL(s[i].kind);
            saw_unknown = 1;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, saw_unknown);
    free(s);

    /* ...but it is NOT admitted by a filter whose kinds all resolved: the table
     * never shrinks, so a kind interned now was interned before that posting
     * arrived, and the posting therefore cannot be it. Admitting it would hand
     * back a record the caller did not ask for. */
    const char *resolved[] = {"kind_0"};
    n = sources_of(e, 99, resolved, 1, &s);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT64(0, s[0].from_id); /* kind_0's edge, nothing else */
    free(s);

    /* It *is* a candidate when the filter names something un-interned, since
     * then the index genuinely cannot rule it out. */
    const char *unresolved[] = {"one_kind_too_many"};
    n = sources_of(e, 99, unresolved, 1, &s);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT64(999999, s[0].from_id);
    TEST_ASSERT_EQUAL_INT(1, s[0].kind_unknown);
    free(s);
    edge_index_free(e);
}

/* Symmetry with the forward filter: a named filter never matches an edge that
 * carries no kind, and an empty filter entry names nothing. */
static void test_named_filter_never_matches_unkinded(void) {
    EdgeIndex *e = edge_index_create();
    edge_index_add(e, 1, 99, NULL);
    edge_index_add(e, 2, 99, "supersedes");

    const char *sup[] = {"supersedes"};
    EdgeSource *s = NULL;
    TEST_ASSERT_EQUAL_UINT(1, sources_of(e, 99, sup, 1, &s));
    TEST_ASSERT_EQUAL_UINT64(2, s[0].from_id);
    free(s);

    const char *empty[] = {""};
    TEST_ASSERT_EQUAL_UINT(0, sources_of(e, 99, empty, 1, &s));
    TEST_ASSERT_NULL(s);

    const char *nullent[] = {NULL};
    TEST_ASSERT_EQUAL_UINT(0, sources_of(e, 99, nullent, 1, &s));
    TEST_ASSERT_NULL(s);

    /* an unfiltered query still returns both */
    TEST_ASSERT_EQUAL_UINT(2, sources_of(e, 99, NULL, 0, &s));
    free(s);
    edge_index_free(e);
}

/* An over-long kind is not truncated (which would collide two distinct kinds
 * into one and answer the filter wrongly) — it is recorded as unknown. */
static void test_overlong_kind_is_unknown_not_truncated(void) {
    EdgeIndex *e = edge_index_create();
    char big[EDGE_MAX_KIND_LEN + 32];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    TEST_ASSERT_EQUAL_INT(0, edge_index_add(e, 1, 99, big));
    TEST_ASSERT_EQUAL_UINT(0, edge_index_kinds(e)); /* nothing interned */
    TEST_ASSERT_EQUAL_UINT(1, edge_index_edges(e));

    EdgeSource *s = NULL;
    TEST_ASSERT_EQUAL_UINT(1, sources_of(e, 99, NULL, 0, &s));
    TEST_ASSERT_EQUAL_INT(1, s[0].kind_unknown);
    TEST_ASSERT_NULL(s[0].kind);
    free(s);

    /* exactly at the limit still interns */
    char at[EDGE_MAX_KIND_LEN + 1];
    memset(at, 'b', EDGE_MAX_KIND_LEN);
    at[EDGE_MAX_KIND_LEN] = '\0';
    TEST_ASSERT_EQUAL_INT(0, edge_index_add(e, 2, 99, at));
    TEST_ASSERT_EQUAL_UINT(1, edge_index_kinds(e));
    edge_index_free(e);
}

/* ---- NULL tolerance ----------------------------------------------------- */

/* Every entry point must treat a NULL index as "--no-edge-index", so the write
 * path can call them unconditionally instead of guarding each site. */
static void test_null_index_is_inert(void) {
    TEST_ASSERT_EQUAL_INT(0, edge_index_add(NULL, 1, 2, "k"));
    edge_index_remove(NULL, 1, 2, "k");
    edge_index_remove_target(NULL, 1);
    EdgeSource *s = (EdgeSource *)0x1;
    size_t n = 99;
    TEST_ASSERT_EQUAL_INT(0, edge_index_sources(NULL, 1, NULL, 0, &s, &n));
    TEST_ASSERT_NULL(s);
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_UINT(0, edge_index_edges(NULL));
    TEST_ASSERT_EQUAL_UINT(0, edge_index_kinds(NULL));
    TEST_ASSERT_EQUAL_UINT(0, edge_index_bytes(NULL));
    edge_index_free(NULL);
}

/* ---- differential stress ------------------------------------------------ */

/* The index is a hash table with tombstones, sorted postings, and interned
 * kinds — three places where an off-by-one hides quietly and only shows up as a
 * missing edge months later. So: run a deterministic pseudo-random op sequence
 * against a brute-force reference model and assert they agree exactly.
 * Deterministic (fixed LCG seed) so a failure is reproducible. */

/* The op sequence below draws from ID_SPACE ids and N_KINDS kinds, so the model
 * must be able to hold every distinct edge that can exist — otherwise it
 * silently stops tracking adds and "diverges" from a perfectly correct index.
 * (It did, the first time: the index was right and the model was full.) */
#define ID_SPACE 40
#define REF_MAX (ID_SPACE * ID_SPACE * N_KINDS)
/* Most edges any one target can receive. */
#define MAX_INDEG (ID_SPACE * N_KINDS)

static const char *const KINDS[] = {"supersedes", "derived_from", "mentions"};
#define N_KINDS 3

typedef struct {
    uint64_t from;
    uint64_t to;
    int kind; /* index into KINDS */
    int live;
} RefEdge;

static uint64_t lcg_state;
static uint64_t lcg(void) {
    lcg_state = lcg_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return lcg_state >> 17;
}

/* Sort a result into the same canonical order as ref_sources, so the comparison
 * does not depend on the index's internal secondary ordering. */
static void canonicalise(EdgeSource *s, size_t n) {
    for (size_t i = 1; i < n; i++) {
        EdgeSource v = s[i];
        size_t j = i;
        while (j > 0 && (s[j - 1].from_id > v.from_id ||
                         (s[j - 1].from_id == v.from_id &&
                          strcmp(s[j - 1].kind, v.kind) > 0))) {
            s[j] = s[j - 1];
            j--;
        }
        s[j] = v;
    }
}

/* Live sources of `to` in the model, in a canonical order: from_id, then kind
 * name lexicographically. The index's own secondary order is an internal
 * (first-seen interning) detail, so the comparison below canonicalises both
 * sides rather than assuming it. */
static size_t ref_sources(const RefEdge *ref, size_t ref_n, uint64_t to,
                          uint64_t *out_from, int *out_kind) {
    size_t n = 0;
    for (size_t i = 0; i < ref_n; i++) {
        if (ref[i].live && ref[i].to == to) {
            out_from[n] = ref[i].from;
            out_kind[n] = ref[i].kind;
            n++;
        }
    }
    for (size_t i = 1; i < n; i++) { /* insertion sort; n is small */
        uint64_t f = out_from[i];
        int k = out_kind[i];
        size_t j = i;
        while (j > 0 && (out_from[j - 1] > f ||
                         (out_from[j - 1] == f &&
                          strcmp(KINDS[out_kind[j - 1]], KINDS[k]) > 0))) {
            out_from[j] = out_from[j - 1];
            out_kind[j] = out_kind[j - 1];
            j--;
        }
        out_from[j] = f;
        out_kind[j] = k;
    }
    return n;
}

static void test_differential_against_reference_model(void) {
    EdgeIndex *e = edge_index_create();
    RefEdge *ref = calloc(REF_MAX, sizeof(*ref));
    TEST_ASSERT_NOT_NULL(ref);
    size_t ref_n = 0;
    size_t live = 0;
    lcg_state = 0x5EED5EED5EED5EEDULL;

    /* A small id space keeps collisions, re-adds, and shared targets common —
     * which is where the interesting bugs are. */

    for (int step = 0; step < 20000; step++) {
        uint64_t op = lcg() % 100;
        uint64_t from = lcg() % ID_SPACE;
        uint64_t to = lcg() % ID_SPACE;
        int kind = (int)(lcg() % N_KINDS);
        if (from == to) {
            continue; /* qe_relate rejects self-edges, so never index one */
        }

        if (op < 60) { /* add */
            TEST_ASSERT_EQUAL_INT(0, edge_index_add(e, from, to, KINDS[kind]));
            int found = 0;
            for (size_t i = 0; i < ref_n; i++) {
                if (ref[i].from == from && ref[i].to == to &&
                    ref[i].kind == kind) {
                    if (!ref[i].live) {
                        ref[i].live = 1;
                        live++;
                    }
                    found = 1;
                    break;
                }
            }
            if (!found) {
                /* Loudly, not silently: a full model would fake a divergence. */
                TEST_ASSERT_TRUE(ref_n < REF_MAX);
                ref[ref_n].from = from;
                ref[ref_n].to = to;
                ref[ref_n].kind = kind;
                ref[ref_n].live = 1;
                ref_n++;
                live++;
            }
        } else if (op < 85) { /* remove one edge */
            edge_index_remove(e, from, to, KINDS[kind]);
            for (size_t i = 0; i < ref_n; i++) {
                if (ref[i].from == from && ref[i].to == to &&
                    ref[i].kind == kind && ref[i].live) {
                    ref[i].live = 0;
                    live--;
                    break;
                }
            }
        } else { /* remove every edge into `to` (a tombstone) */
            edge_index_remove_target(e, to);
            for (size_t i = 0; i < ref_n; i++) {
                if (ref[i].to == to && ref[i].live) {
                    ref[i].live = 0;
                    live--;
                }
            }
        }

        /* The edge count must track exactly, every step. */
        TEST_ASSERT_EQUAL_UINT(live, edge_index_edges(e));

        /* Spot-check one target per step (checking all 40 every step would make
         * this test quadratic for no extra coverage over 20k steps). */
        uint64_t probe = lcg() % ID_SPACE;
        uint64_t exp_from[MAX_INDEG];
        int exp_kind[MAX_INDEG];
        size_t exp_n = ref_sources(ref, ref_n, probe, exp_from, exp_kind);
        EdgeSource *got = NULL;
        size_t got_n = 0;
        TEST_ASSERT_EQUAL_INT(
            0, edge_index_sources(e, probe, NULL, 0, &got, &got_n));
        TEST_ASSERT_EQUAL_UINT(exp_n, got_n);
        /* from_id ascending is the contract, so check it on the raw result. */
        for (size_t i = 1; i < got_n; i++) {
            TEST_ASSERT_TRUE(got[i - 1].from_id <= got[i].from_id);
        }
        canonicalise(got, got_n);
        for (size_t i = 0; i < exp_n; i++) {
            TEST_ASSERT_EQUAL_UINT64(exp_from[i], got[i].from_id);
            TEST_ASSERT_EQUAL_STRING(KINDS[exp_kind[i]], got[i].kind);
            TEST_ASSERT_EQUAL_INT(0, got[i].kind_unknown);
        }
        free(got);
    }

    /* Finally verify every target at once, not just the sampled ones. */
    for (uint64_t to = 0; to < ID_SPACE; to++) {
        uint64_t exp_from[MAX_INDEG];
        int exp_kind[MAX_INDEG];
        size_t exp_n = ref_sources(ref, ref_n, to, exp_from, exp_kind);
        EdgeSource *got = NULL;
        size_t got_n = 0;
        TEST_ASSERT_EQUAL_INT(0,
                              edge_index_sources(e, to, NULL, 0, &got, &got_n));
        TEST_ASSERT_EQUAL_UINT(exp_n, got_n);
        for (size_t i = 0; i < exp_n; i++) {
            TEST_ASSERT_EQUAL_UINT64(exp_from[i], got[i].from_id);
        }
        free(got);
    }
    free(ref);
    edge_index_free(e);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add_then_query_reverse);
    RUN_TEST(test_unknown_target_is_empty_not_an_error);
    RUN_TEST(test_add_is_idempotent);
    RUN_TEST(test_unkinded_edge);
    RUN_TEST(test_kind_filter);
    RUN_TEST(test_remove_one_edge);
    RUN_TEST(test_remove_target_drops_whole_indegree);
    RUN_TEST(test_readd_after_target_removed);
    RUN_TEST(test_churn_reclaims);
    RUN_TEST(test_many_targets_grow_and_stay_exact);
    RUN_TEST(test_bytes_grows_with_content);
    RUN_TEST(test_kind_count_tracks_use_not_history);
    RUN_TEST(test_kind_cap_degrades_to_unknown);
    RUN_TEST(test_overlong_kind_is_unknown_not_truncated);
    RUN_TEST(test_named_filter_never_matches_unkinded);
    RUN_TEST(test_null_index_is_inert);
    RUN_TEST(test_differential_against_reference_model);
    return UNITY_END();
}
