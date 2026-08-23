/* Deterministic inference (ROADMAP 5.3): the closures, computed purely.
 *
 * No database, no threads, no locks, no writes. Given a snapshot of one
 * namespace's live facts and the registry that declares their relational
 * properties, infer_run returns the conclusions that follow. *When* this runs,
 * *where* the facts come from and *what becomes of* the conclusions all belong
 * to the maintenance job; this file is only the arithmetic.
 *
 * That split is deliberate. A rule engine that cannot be exercised without a
 * server is one whose cycle, dedup and cap behaviour never gets pinned down,
 * and those are exactly the parts that go wrong quietly.
 *
 * Three rules, all driven by declarations the 5.2 registry already validates,
 * and all restricted to `object: "id"` predicates (which the registry enforces
 * at load, since each relates a subject to a subject):
 *
 *   transitive   (a p b), (b p c)          =>  (a p c)
 *   symmetric    (a p b)                   =>  (b p a)
 *   inverse_of   (a p b), inverse_of(p,q)  =>  (b q a)
 *
 * With no registry, nothing declares anything and nothing is derivable — which
 * is the default, and is why an unconfigured server sees no new records.
 *
 * See docs/inference-design.md §4.1.
 */
#ifndef AEGISDB_INFERENCE_H
#define AEGISDB_INFERENCE_H

#include <stddef.h>
#include <stdint.h>

#include "aegisdb/predicate_registry.h"
#include "aegisdb/record.h"

/* One live fact in the input snapshot: the triple, plus what the *asserting
 * record* contributes to a conclusion drawn from it. Strings are borrowed and
 * must outlive the InferResult, which borrows them in turn. */
typedef struct {
    uint64_t record_id; /* the record asserting it; becomes a premise id */
    uint64_t subject;
    const char *predicate;
    FactKind object_kind;
    uint64_t object_id;     /* object_kind == FACT_OBJ_ID */
    const char *object_str; /* object_kind == FACT_OBJ_STRING */
    uint16_t depth;   /* asserting record's derivation depth; 0 asserted */
    float confidence; /* asserting record's confidence */
} InferFact;

/* One conclusion: the triple to write, and the provenance to write with it.
 * `predicate` and `object_str` are borrowed from the inputs or the registry. */
/* One justification for a conclusion. */
typedef struct {
    DerivRule rule;
    uint16_t depth;
    uint64_t premises[DERIV_MAX_PREMISES];
    uint8_t premise_count;
} InferRoute;

/* One conclusion: the triple to write, and every way this pass found to reach
 * it. Support is disjunctive — a conclusion stands while any one route's
 * premises are live — so a pass that found two routes must report both, or
 * retraction will drop a conclusion that is still supported. */
typedef struct {
    /* Ordered by premise ids, and capped at DERIV_MAX_ROUTES: past that the
     * lowest-ordered routes are kept, which can only cost a re-derivation. */
    InferRoute routes[DERIV_MAX_ROUTES];
    size_t route_count;
    /* The best route's product, floored — the strongest justification found,
     * not the first. */
    float confidence;

    uint64_t subject;
    const char *predicate;
    FactKind object_kind;
    uint64_t object_id;
    const char *object_str;
} InferConclusion;

#define INFER_DEFAULT_MAX_DEPTH 4
#define INFER_DEFAULT_CONFIDENCE_FLOOR 0.1F
/* Candidate conclusions considered per pass. This is the cap that actually
 * bounds a tick: a corpus whose closure is already materialized offers the
 * same candidates every pass and keeps none of them, so a cap on conclusions
 * *written* never fires while the work still grows with the corpus. */
#define INFER_DEFAULT_MAX_CANDIDATES 1000000

typedef struct {
    /* A conclusion deeper than this is not drawn. Depth is one past the deepest
     * premise, so this caps chain length rather than pass count. 0 = default. */
    uint16_t max_depth;
    /* Stop after this many *new* conclusions, setting `truncated`. Duplicates
     * do not count against it, so meeting the cap and then seeing nothing but
     * duplicates is not reported as deferred work. 0 = unlimited. */
    size_t max_conclusions;
    /* Stop after considering this many candidates, setting `truncated`. Unlike
     * max_conclusions this bounds *work*, so it is what keeps a tick's duration
     * off the corpus's shape. 0 = INFER_DEFAULT_MAX_CANDIDATES. */
    size_t max_candidates;
    /* Where in `facts` rule application begins; it wraps. A budgeted pass that
     * always started at 0 would examine the same prefix forever and never
     * reach the rest, so a caller that sees `truncated` should advance this by
     * `candidates_examined` worth of facts on the next pass. Indexing the dedup
     * set is always complete regardless. */
    size_t start_index;
    /* Floor under the confidence product; <= 0 = default. Note this can raise a
     * conclusion above its premises — see infer_run. */
    float confidence_floor;
} InferOpts;

typedef struct {
    InferConclusion *items;
    size_t n;
    /* A cap stopped the pass with candidates left. Not an error: the caller
     * runs again. Reported rather than swallowed, because a pass that never
     * reaches fixpoint is survivable but worth being able to see. */
    int truncated;
    /* Candidates considered, whether kept, deduped or too deep. The work the
     * pass actually did, and what a caller advances `start_index` by. */
    size_t candidates_examined;
} InferResult;

/* Draw every conclusion derivable in one pass over `facts`.
 *
 * **One pass, not a fixpoint.** Conclusions are drawn from the input facts
 * only, never from each other, so a chain a->b->c->d yields a->c and b->d here
 * and a->d only once those are facts the next pass can see. Full closure
 * emerges over successive passes, which is the point: running to fixpoint would
 * make one tick's duration a function of corpus shape, and that is how a
 * background job becomes an outage (design doc §5).
 *
 * `facts` must be the **complete** live fact set for one namespace. Dedup is
 * against that set (plus conclusions drawn earlier in the same pass), so a
 * partial snapshot re-derives facts that already exist. It must also be one
 * namespace only: joining a premise from one tenant to a premise from another
 * would conclude something that exists in neither, and this function cannot
 * tell them apart — the caller scopes the input.
 *
 * Neither the conclusion set nor the provenance recorded for a conclusion
 * depends on the order of `facts`: when several routes reach the same triple,
 * the one with the lowest premise ids is the one recorded, not the one seen
 * first. What *does* depend on order is which conclusions survive
 * `max_conclusions` and `max_candidates`, so pass a stable order (record id)
 * if reproducibility across passes matters.
 *
 * A conclusion records **every** route this pass found to it, up to
 * DERIV_MAX_ROUTES, because support is disjunctive: retraction has to be able
 * to ask whether *any* justification still stands. Past the cap the
 * lowest-ordered routes are kept, which can cost a conclusion a retraction and
 * a re-derivation but never makes one wrong.
 *
 * Confidence is the product of the premises' with a floor, so it is *not*
 * monotonic along a chain: the floor can raise a conclusion above the premises
 * it came from. That is deliberate (§8), and it is a heuristic, not a
 * probability.
 *
 * Returns 0 with *out populated (free with infer_result_free), or -1 on
 * allocation failure, in which case *out is zeroed. A NULL or empty registry
 * yields zero conclusions, not an error. */
int infer_run(const InferFact *facts, size_t nfacts,
              const PredicateRegistry *reg, const InferOpts *opts,
              InferResult *out);

void infer_result_free(InferResult *r);

#endif /* AEGISDB_INFERENCE_H */
