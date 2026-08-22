/* AegisDB MemoryRecord and Relationship definitions (T006, extended T046). */
#ifndef AEGISDB_RECORD_H
#define AEGISDB_RECORD_H

#include <stddef.h>
#include <stdint.h>

#include "aegisdb/types.h"

/* Directed edge between two persisted records (Phase 4 / US5). */
typedef struct {
    uint64_t from_id;
    uint64_t to_id;
    char *kind; /* owned, may be NULL */
} Relationship;

/* What kind of object a fact's `o` position holds (ROADMAP 5.2). Values are
 * stable: persisted in the log encoding.
 *
 * A numeric object is deliberately absent. Nothing exact can be asserted about
 * a float, and it would make the (predicate, object) index key on IEEE-754 bits
 * — where `1` and `1.0` collide but `0.1 + 0.2` and `0.3` do not. Value 3 is
 * reserved for it should a predicate ever genuinely need one; adding it means a
 * codec bump, because a decoder that does not know a kind must refuse the frame
 * rather than guess at its length. */
typedef enum {
    FACT_NONE = 0,       /* the record asserts no machine-readable fact */
    FACT_OBJ_ID = 1,     /* object is a record id */
    FACT_OBJ_STRING = 2, /* object is a literal string */
} FactKind;

/* An optional machine-readable rendering of what a record asserts, alongside
 * the prose that stays in `data`. Neither is derived from the other: a writer
 * supplies both, and the prose remains what a human (or a model) reads.
 *
 * `kind == FACT_NONE` is the default and means the record carries no fact — in
 * which case it encodes byte-for-byte as it did before facts existed. */
typedef struct {
    FactKind kind;
    uint64_t subject;   /* record id the fact is about */
    char *predicate;    /* owned; non-NULL exactly when kind != FACT_NONE */
    uint64_t object_id; /* kind == FACT_OBJ_ID */
    char *object_str;   /* owned; kind == FACT_OBJ_STRING */
} Fact;

/* Which inference rule produced a derived record (ROADMAP 5.3). Values are
 * stable: persisted in the log encoding. A decoder that meets a rule it does
 * not know refuses the frame rather than guessing, for the same reason an
 * unknown FactKind is refused — except here the length is fixed, so the refusal
 * is about meaning rather than framing: a record whose provenance cannot be
 * named is not one to hand a caller as if it were understood. */
typedef enum {
    DERIV_NONE = 0,       /* asserted, not derived — the default */
    DERIV_TRANSITIVE = 1, /* (a p b), (b p c) => (a p c) */
    DERIV_SYMMETRIC = 2,  /* (a p b) => (b p a) */
    DERIV_INVERSE = 3,    /* (a p b), inverse_of(p, q) => (b q a) */
} DerivRule;

/* No rule here needs more than two premises; the cap is generous so a future
 * one has room, and small enough that the array is never a memory concern. */
#define DERIV_MAX_PREMISES 8

/* How a derived record came to exist (ROADMAP 5.3): which rule fired, how deep
 * in a chain it sits, and which records it was concluded from.
 *
 * `premises` duplicates the record's `derived_from` relationships on purpose.
 * The edges are what a walk uses; this array is what *survives* the edges — a
 * tombstoned premise takes its incoming edge out of the reverse index, and a
 * derived record that kept its lineage only in the graph would lose the ability
 * to explain itself at exactly the moment the explanation matters most. It is
 * also what lets recovery reconcile every derived record against its premises
 * without consulting an index at all (docs/inference-design.md §6).
 *
 * `rule == DERIV_NONE` is the default and means the record was asserted, not
 * derived — in which case it encodes exactly as it did before 5.3. */
typedef struct {
    DerivRule rule;
    uint16_t depth;     /* 0 for a conclusion drawn only from assertions */
    uint64_t *premises; /* owned; non-NULL exactly when rule != DERIV_NONE */
    size_t premise_count;
} Derivation;

/* Primary persisted (or RAM-held, for working) entity.
 * All pointer fields are owned by the record and freed by record_free(). */
typedef struct {
    uint64_t id;
    MemoryType type;
    uint64_t created; /* epoch ms */
    uint64_t updated; /* epoch ms */
    float importance; /* [0,1], default 0.0 */
    float confidence; /* [0,1], default 1.0 */

    char *agent_id; /* owned, may be NULL (Phase 4) */

    char **tags; /* owned array of owned strings */
    size_t tag_count;

    /* Embeddings: `vec_count` vectors of `embedding_dim` floats each, stored
     * contiguously (vector i at embedding + i*embedding_dim). vec_count is 0
     * when there is no embedding, 1 for the common single-vector case (#85). */
    float *embedding; /* owned, may be NULL (Phase 3) */
    size_t embedding_dim;
    size_t vec_count;

    Relationship *relationships; /* owned, may be NULL (Phase 4) */
    size_t rel_count;

    Fact fact; /* optional typed assertion (ROADMAP 5.2); zeroed = none */

    Derivation derivation; /* set only on a derived record (5.3); zeroed = none.
                            * Server-written: `insert` refuses a client-supplied
                            * one, because forgeable provenance is worse than
                            * none. */

    void *data; /* owned opaque payload */
    size_t data_len;

    uint64_t expires_at; /* working memory only; 0 = none */
    int deleted;         /* tombstone marker for compaction */
} MemoryRecord;

/* Zero-initialise a record with field defaults (confidence=1.0). */
void record_init(MemoryRecord *r);

/* Free all owned members and zero the struct. Safe on zeroed records. */
void record_free(MemoryRecord *r);

/* Deep-copy src into a freshly allocated record (caller frees). NULL on OOM. */
MemoryRecord *record_clone(const MemoryRecord *src);

/* Replace the tag set with a copy of `tags` (n entries). Returns 0/-1. */
int record_set_tags(MemoryRecord *r, const char *const *tags, size_t n);

/* Append a relationship (copying `kind`). Returns 0/-1. */
int record_add_relationship(MemoryRecord *r, uint64_t from_id, uint64_t to_id,
                            const char *kind);

/* Set (or clear) the record's fact, copying the strings and releasing whatever
 * was there. Pass FACT_NONE to clear; `predicate` is required otherwise, and
 * `object_str` is required for FACT_OBJ_STRING and ignored for FACT_OBJ_ID.
 * Returns 0, or -1 on a bad argument combination or allocation failure (in
 * which case the record's previous fact is left intact). */
int record_set_fact(MemoryRecord *r, FactKind kind, uint64_t subject,
                    const char *predicate, uint64_t object_id,
                    const char *object_str);

/* Set (or clear) the record's derivation, copying the premise ids and releasing
 * whatever was there. Pass DERIV_NONE to clear; otherwise `premises` must hold
 * 1..DERIV_MAX_PREMISES ids. Returns 0, or -1 on a bad argument combination or
 * allocation failure (in which case the previous derivation is left intact). */
int record_set_derivation(MemoryRecord *r, DerivRule rule, uint16_t depth,
                          const uint64_t *premises, size_t n);

/* Codec versions of the on-disk record encoding. v1 held a single embedding;
 * v2 added multi-vector; v3 added the optional Fact above; v4 added the optional
 * Derivation. A record is encoded with the *lowest* version that can represent
 * it, so a fact-less record is still v2, a derivation-less one with a fact is
 * still v3, and an existing log stays byte-compatible.
 *
 * RECORD_CODEC_MAX is what a build can read, and is exchanged in the
 * replication handshake: a primary that is about to ship a frame newer than its
 * replica can decode should say so, rather than let the replica fail per frame
 * with nothing to go on. */
#define RECORD_CODEC_V2 2
#define RECORD_CODEC_V3 3
#define RECORD_CODEC_V4 4
#define RECORD_CODEC_MAX RECORD_CODEC_V4

/* Binary (little-endian, length-prefixed) codec used by the append-only log.
 * record_encode allocates *out (free with free()). record_decode fills *out
 * which must then be released with record_free(). Both return 0/-1. */
int record_encode(const MemoryRecord *r, uint8_t **out, size_t *out_len);
int record_decode(const uint8_t *buf, size_t len, MemoryRecord *out);

#endif /* AEGISDB_RECORD_H */