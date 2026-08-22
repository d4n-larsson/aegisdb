/* Predicate registry (ROADMAP 5.2): the declared vocabulary for typed facts.
 *
 * A write path that mints predicates freely produces a symbol soup no rule can
 * ever fire on — the standard way a model-built knowledge graph dies. The
 * registry is the contract that prevents it: a small JSON file, authored
 * deliberately, that says which predicates exist and what shape their objects
 * take. 5.4's extractor is prompted against it; 5.3's closures read the
 * relational properties it declares.
 *
 * Loaded once at startup and immutable thereafter, so lookups need no lock.
 *
 * With no registry configured, any predicate is accepted. A server that has not
 * opted into a vocabulary should not be broken by this feature, and the
 * strictness is worth nothing until someone is writing facts on purpose.
 */
#ifndef AEGISDB_PREDICATE_REGISTRY_H
#define AEGISDB_PREDICATE_REGISTRY_H

#include <stddef.h>

#include "aegisdb/record.h"

/* What a predicate's object position must hold. Declaring this is required:
 * a predicate that accepted either an id or a literal would make a
 * (predicate, object) lookup mean two different things. */
typedef enum {
    PRED_OBJ_ID = 1,     /* {"object": "id"} — a record reference */
    PRED_OBJ_STRING = 2, /* {"object": "string"} — a literal */
} PredObjKind;

/* One predicate's declaration. The relational properties are *declared* here
 * and consumed by 5.3; 5.2 validates them for coherence but draws no
 * conclusions from them. */
typedef struct {
    const char *name;
    PredObjKind object;
    int single_valued; /* {"cardinality": "one"}; default many */
    int symmetric;
    int transitive;
    const char *inverse_of;        /* NULL when unset; mutual by validation */
    const char *const *mutex_with; /* NULL when unset */
    size_t mutex_count;
} PredicateSpec;

typedef struct PredicateRegistry PredicateRegistry;

/* Load and fully validate a registry file. Returns NULL on any problem and
 * writes a human-readable reason into `err` (which names the offending
 * predicate, so a startup failure is actionable). Validation is deliberately
 * strict — an unknown key or a one-sided `inverse_of` is a typo, and a typo in
 * a vocabulary file is worth failing startup over rather than discovering as a
 * rule that never fires. */
PredicateRegistry *predicate_registry_load(const char *path, char *err,
                                           size_t errlen);
void predicate_registry_free(PredicateRegistry *r);

/* Declared predicates. 0 for a NULL registry. */
size_t predicate_registry_count(const PredicateRegistry *r);

/* The declaration for `predicate`, or NULL if it is not declared (or the
 * registry is NULL). */
const PredicateSpec *predicate_registry_get(const PredicateRegistry *r,
                                            const char *predicate);

/* May a fact use this predicate with this object kind? Returns 0 if allowed.
 * Returns -1 otherwise and writes the reason into `err`. A NULL registry allows
 * everything, which is what an unconfigured server does. */
int predicate_registry_check(const PredicateRegistry *r, const char *predicate,
                             FactKind okind, char *err, size_t errlen);

#endif /* AEGISDB_PREDICATE_REGISTRY_H */
