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
typedef struct {
    DerivRule rule;
    uint16_t depth;
    float confidence;
    uint64_t premises[DERIV_MAX_PREMISES];
    size_t premise_count;

    uint64_t subject;
    const char *predicate;
    FactKind object_kind;
    uint64_t object_id;
    const char *object_str;
} InferConclusion;

#define INFER_DEFAULT_MAX_DEPTH 4
#define INFER_DEFAULT_CONFIDENCE_FLOOR 0.1F

typedef struct {
    /* A conclusion deeper than this is not drawn. Depth is one past the deepest
     * premise, so this caps chain length rather than pass count. 0 = default. */
    uint16_t max_depth;
    /* Stop after this many conclusions, setting `truncated`. 0 = unlimited. */
    size_t max_conclusions;
    /* Floor under the confidence product; <= 0 = default. */
    float confidence_floor;
} InferOpts;

typedef struct {
    InferConclusion *items;
    size_t n;
    /* A cap stopped the pass with candidates left. Not an error: the caller
     * runs again. Reported rather than swallowed, because a pass that never
     * reaches fixpoint is survivable but worth being able to see. */
    int truncated;
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
 * The conclusion *set* does not depend on the order of `facts`, but which
 * conclusions survive `max_conclusions` does. Pass a stable order (record id)
 * if reproducibility across passes matters.
 *
 * Returns 0 with *out populated (free with infer_result_free), or -1 on
 * allocation failure, in which case *out is zeroed. A NULL or empty registry
 * yields zero conclusions, not an error. */
int infer_run(const InferFact *facts, size_t nfacts,
              const PredicateRegistry *reg, const InferOpts *opts,
              InferResult *out);

void infer_result_free(InferResult *r);

#endif /* AEGISDB_INFERENCE_H */
