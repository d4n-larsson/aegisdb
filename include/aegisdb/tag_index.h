/* Inverted tag index (T028): tag -> sorted list of ids. */
#ifndef AEGISDB_TAG_INDEX_H
#define AEGISDB_TAG_INDEX_H

#include <stddef.h>
#include <stdint.h>

typedef struct TagIndex TagIndex;

TagIndex *tag_index_create(void);
void tag_index_free(TagIndex *t);

int tag_index_add(TagIndex *t, const char *tag, uint64_t id);
void tag_index_remove(TagIndex *t, const char *tag, uint64_t id);

/* Query `n` tags. match_all != 0 -> intersection; else union. Results are
 * sorted ascending ids. Allocates *out (free with free()). Returns 0/-1. */
int tag_index_query(const TagIndex *t, const char *const *tags, size_t n,
                    int match_all, uint64_t **out, size_t *out_n);

/* Number of distinct tags currently indexed. */
size_t tag_index_count(const TagIndex *t);

#endif /* AEGISDB_TAG_INDEX_H */