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

/* Binary (little-endian, length-prefixed) codec used by the append-only log.
 * record_encode allocates *out (free with free()). record_decode fills *out
 * which must then be released with record_free(). Both return 0/-1. */
int record_encode(const MemoryRecord *r, uint8_t **out, size_t *out_len);
int record_decode(const uint8_t *buf, size_t len, MemoryRecord *out);

#endif /* AEGISDB_RECORD_H */