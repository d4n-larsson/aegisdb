/* Inverted tag index (T028): hash map of tag -> sorted id list. */
#include "aegisdb/tag_index.h"

#include <stdlib.h>
#include <string.h>

typedef struct TagNode {
    char *tag;
    uint64_t *ids; /* sorted ascending */
    size_t n;
    size_t cap;
    struct TagNode *next;
} TagNode;

#define NBUCKETS 1024

struct TagIndex {
    TagNode *buckets[NBUCKETS];
};

static size_t hash_str(const char *s) {
    size_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h % NBUCKETS;
}

TagIndex *tag_index_create(void) {
    TagIndex *t = calloc(1, sizeof(*t));
    return t;
}

size_t tag_index_count(const TagIndex *t) {
    if (!t) return 0;
    size_t n = 0;
    for (size_t i = 0; i < NBUCKETS; i++)
        for (const TagNode *node = t->buckets[i]; node; node = node->next) n++;
    return n;
}

/* Approximate resident bytes: the bucket table + each tag node (struct, its tag
 * string, and its id posting array). Excludes allocator overhead. */
size_t tag_index_bytes(const TagIndex *t) {
    if (!t) return 0;
    size_t total = sizeof(*t);
    for (size_t i = 0; i < NBUCKETS; i++)
        for (const TagNode *node = t->buckets[i]; node; node = node->next)
            total += sizeof(*node) + (node->tag ? strlen(node->tag) + 1 : 0) +
                     node->cap * sizeof(uint64_t);
    return total;
}

void tag_index_free(TagIndex *t) {
    if (!t) return;
    for (size_t i = 0; i < NBUCKETS; i++) {
        TagNode *n = t->buckets[i];
        while (n) {
            TagNode *nx = n->next;
            free(n->tag);
            free(n->ids);
            free(n);
            n = nx;
        }
    }
    free(t);
}

static TagNode *find_node(const TagIndex *t, const char *tag) {
    size_t b = hash_str(tag);
    for (TagNode *n = t->buckets[b]; n; n = n->next)
        if (strcmp(n->tag, tag) == 0) return n;
    return NULL;
}

/* Binary search for id; returns index of first element >= id. */
static size_t id_lower_bound(const uint64_t *ids, size_t n, uint64_t id) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ids[mid] < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int tag_index_add(TagIndex *t, const char *tag, uint64_t id) {
    size_t b = hash_str(tag);
    TagNode *n = find_node(t, tag);
    if (!n) {
        n = calloc(1, sizeof(*n));
        if (!n) return -1;
        n->tag = strdup(tag);
        if (!n->tag) {
            free(n);
            return -1;
        }
        n->next = t->buckets[b];
        t->buckets[b] = n;
    }
    size_t pos = id_lower_bound(n->ids, n->n, id);
    if (pos < n->n && n->ids[pos] == id) return 0; /* dedupe */
    if (n->n == n->cap) {
        size_t cap = n->cap ? n->cap * 2 : 8;
        uint64_t *ni = realloc(n->ids, cap * sizeof(uint64_t));
        if (!ni) return -1;
        n->ids = ni;
        n->cap = cap;
    }
    if (pos < n->n)
        memmove(&n->ids[pos + 1], &n->ids[pos],
                (n->n - pos) * sizeof(uint64_t));
    n->ids[pos] = id;
    n->n++;
    return 0;
}

void tag_index_remove(TagIndex *t, const char *tag, uint64_t id) {
    size_t b = hash_str(tag);
    TagNode *prev = NULL;
    for (TagNode *n = t->buckets[b]; n; prev = n, n = n->next) {
        if (strcmp(n->tag, tag) != 0) continue;
        size_t pos = id_lower_bound(n->ids, n->n, id);
        if (pos < n->n && n->ids[pos] == id) {
            memmove(&n->ids[pos], &n->ids[pos + 1],
                    (n->n - pos - 1) * sizeof(uint64_t));
            n->n--;
        }
        /* Reclaim a now-empty node instead of leaving it in the chain, where
         * repeated add/remove of distinct tags would grow the bucket unbounded. */
        if (n->n == 0) {
            if (prev)
                prev->next = n->next;
            else
                t->buckets[b] = n->next;
            free(n->ids);
            free(n->tag);
            free(n);
        }
        return;
    }
}

int tag_index_query(const TagIndex *t, const char *const *tags, size_t n,
                    int match_all, uint64_t **out, size_t *out_n) {
    if (n == 0) {
        *out = NULL;
        *out_n = 0;
        return 0;
    }
    /* Gather node id-lists; absent tags contribute empty sets. */
    size_t cap = 16, cnt = 0;
    uint64_t *acc = malloc(cap * sizeof(uint64_t));
    if (!acc) return -1;

    if (match_all) {
        /* Intersection: start from the smallest list, keep ids present in all. */
        const TagNode *base = find_node(t, tags[0]);
        if (!base) {
            *out = acc;
            *out_n = 0;
            return 0;
        }
        for (size_t i = 0; i < base->n; i++) {
            uint64_t id = base->ids[i];
            int in_all = 1;
            for (size_t j = 1; j < n; j++) {
                const TagNode *nj = find_node(t, tags[j]);
                if (!nj) { in_all = 0; break; }
                size_t pos = id_lower_bound(nj->ids, nj->n, id);
                if (!(pos < nj->n && nj->ids[pos] == id)) { in_all = 0; break; }
            }
            if (in_all) {
                if (cnt == cap) {
                    cap *= 2;
                    uint64_t *na = realloc(acc, cap * sizeof(uint64_t));
                    if (!na) { free(acc); return -1; }
                    acc = na;
                }
                acc[cnt++] = id;
            }
        }
    } else {
        /* Union via sorted merge into acc (kept sorted + deduped). */
        for (size_t j = 0; j < n; j++) {
            const TagNode *nj = find_node(t, tags[j]);
            if (!nj) continue;
            for (size_t i = 0; i < nj->n; i++) {
                uint64_t id = nj->ids[i];
                size_t pos = id_lower_bound(acc, cnt, id);
                if (pos < cnt && acc[pos] == id) continue;
                if (cnt == cap) {
                    cap *= 2;
                    uint64_t *na = realloc(acc, cap * sizeof(uint64_t));
                    if (!na) { free(acc); return -1; }
                    acc = na;
                }
                if (pos < cnt)
                    memmove(&acc[pos + 1], &acc[pos],
                            (cnt - pos) * sizeof(uint64_t));
                acc[pos] = id;
                cnt++;
            }
        }
    }
    *out = acc;
    *out_n = cnt;
    return 0;
}