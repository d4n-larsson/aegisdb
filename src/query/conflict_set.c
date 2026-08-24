/* The contradictions the last inference pass found — see conflict_set.h. */
#include "aegisdb/conflict_set.h"

#include <stdlib.h>
#include <string.h>

struct ConflictSet {
    ConflictPair *items;
    size_t count;
    size_t cap; /* CONFLICT_SET_MAX; fixed, so a pass cannot grow it */
    int truncated;
};

ConflictSet *conflict_set_create(void) {
    ConflictSet *cs = calloc(1, sizeof(*cs));
    if (!cs) {
        return NULL;
    }
    cs->items = calloc(CONFLICT_SET_MAX, sizeof(*cs->items));
    if (!cs->items) {
        free(cs);
        return NULL;
    }
    cs->cap = CONFLICT_SET_MAX;
    return cs;
}

void conflict_set_free(ConflictSet *cs) {
    if (!cs) {
        return;
    }
    free(cs->items);
    free(cs);
}

void conflict_set_clear(ConflictSet *cs) {
    if (!cs) {
        return;
    }
    cs->count = 0;
    cs->truncated = 0;
}

/* Copy `src` into a fixed field, truncating rather than refusing.
 *
 * Every one of these is already bounded at the write path — a predicate at 64
 * bytes by `insert`, a namespace at MAX_AGENT_ID — so truncation here is
 * unreachable in practice. It is a belt on a brace: this is a report, and a
 * shortened predicate name in a report is a far smaller failure than dropping
 * the contradiction it names. */
static void copy_bounded(char *dst, size_t dstsz, const char *src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dstsz) {
        n = dstsz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int conflict_set_add(ConflictSet *cs, uint64_t a, uint64_t b, const char *ns,
                     const char *predicate_a, const char *predicate_b,
                     const char *reason) {
    if (!cs || a == 0 || b == 0 || a == b) {
        return -1;
    }
    /* Normalized so a pair has one identity regardless of which record the
     * scan reached first. Without it the same contradiction found by the
     * cardinality scan and by a self-referential mutex declaration would read
     * as two, and an adjudicator would spend two model calls to tombstone one
     * record — the second finding its loser already gone. */
    if (a > b) {
        uint64_t t = a;
        a = b;
        b = t;
        const char *tp = predicate_a;
        predicate_a = predicate_b;
        predicate_b = tp;
    }
    /* Linear, because the set is small by construction and a hash table here
     * would be a second thing to get wrong for a loop that runs at most
     * CONFLICT_SET_MAX times per tick. */
    for (size_t i = 0; i < cs->count; i++) {
        if (cs->items[i].a == a && cs->items[i].b == b) {
            return 1; /* already recorded this tick */
        }
    }
    if (cs->count >= cs->cap) {
        /* The gauge is counted, not stored, so it stays exact past this point.
         * Only the list is short, and it says so. */
        cs->truncated = 1;
        return -1;
    }
    ConflictPair *p = &cs->items[cs->count++];
    p->a = a;
    p->b = b;
    copy_bounded(p->ns, sizeof(p->ns), ns);
    copy_bounded(p->predicate_a, sizeof(p->predicate_a), predicate_a);
    copy_bounded(p->predicate_b, sizeof(p->predicate_b), predicate_b);
    copy_bounded(p->reason, sizeof(p->reason), reason);
    return 0;
}

size_t conflict_set_count(const ConflictSet *cs) { return cs ? cs->count : 0; }

int conflict_set_truncated(const ConflictSet *cs) {
    return cs ? cs->truncated : 0;
}

size_t conflict_set_list(const ConflictSet *cs, const char *ns,
                         ConflictPair *out, size_t max, size_t *total) {
    size_t matched = 0;
    size_t written = 0;
    /* `out` may be NULL when `max` is 0: that is the "how many are there?"
     * probe, which fills *total and writes nothing. Refusing it here made the
     * probe answer 0 no matter how many conflicts the pass had found — a wrong
     * all-clear, which is the one answer this whole module exists to avoid. */
    if (!cs || (max > 0 && !out)) {
        if (total) {
            *total = 0;
        }
        return 0;
    }
    /* An empty or NULL `ns` means "every namespace", which only a global token
     * ever reaches: the handler passes the caller's own scope otherwise, so
     * tenant isolation over this list is the isolation already shipped rather
     * than a second mechanism. */
    int all = !ns || !*ns;
    for (size_t i = 0; i < cs->count; i++) {
        if (!all && strcmp(cs->items[i].ns, ns) != 0) {
            continue;
        }
        matched++;
        if (written < max) {
            out[written++] = cs->items[i];
        }
    }
    if (total) {
        *total = matched;
    }
    return written;
}
