/* Per-record usage feedback: recall count + last-recalled time. */
#include "aegisdb/usage_index.h"

#include <stdlib.h>
#include <string.h>

#include "aegisdb/endian.h"
#include "aegisdb/hash_mix.h"

#define USG_SLOT_EMPTY 0
#define USG_SLOT_LIVE 1
#define USG_SLOT_DEAD 2

typedef struct {
    uint64_t id;
    atomic_uint_fast32_t count;   /* recalls observed */
    atomic_uint_fast64_t last_ms; /* epoch ms of the most recent recall */
    uint8_t state;
} UsageEntry;

struct UsageIndex {
    UsageEntry *slots;
    size_t cap;  /* power of two; 0 until the first insert */
    size_t live; /* tracked records */
    size_t used; /* live + dead, for the load factor */
};

/* --- checkpoint format ---------------------------------------------------
 * ["AUSG"][u32 version][u64 entry count] then `count` × (u64 id, u32 recalls,
 * u64 last_ms). Little-endian throughout, via the endian.h codec. */
#define USG_MAGIC "AUSG"
#define USG_VERSION 1u
#define USG_HDR 16
#define USG_ENTRY 20

/* The slot holding `id`, or NULL. Hands back a mutable slot from a const index:
 * the read path mutates only the atomics inside it, and the table's structure is
 * pinned by the caller's read lock. */
static UsageEntry *slot_of(const UsageIndex *u, uint64_t id) {
    if (!u || !u->slots) {
        return NULL;
    }
    size_t mask = u->cap - 1;
    size_t i = (size_t)(mix64(id) & mask);
    for (size_t probe = 0; probe <= mask; probe++) {
        UsageEntry *e = &u->slots[(i + probe) & mask];
        if (e->state == USG_SLOT_EMPTY) {
            return NULL;
        }
        if (e->state == USG_SLOT_LIVE && e->id == id) {
            return e;
        }
    }
    return NULL;
}

static int grow(UsageIndex *u) {
    size_t ncap = u->cap ? u->cap * 2 : 256;
    UsageEntry *ns = calloc(ncap, sizeof(*ns));
    if (!ns) {
        return -1;
    }
    size_t mask = ncap - 1;
    for (size_t i = 0; i < u->cap; i++) {
        if (u->slots[i].state != USG_SLOT_LIVE) {
            continue; /* rehash drops tombstones */
        }
        size_t j = (size_t)(mix64(u->slots[i].id) & mask);
        while (ns[j].state == USG_SLOT_LIVE) {
            j = (j + 1) & mask;
        }
        ns[j].id = u->slots[i].id;
        ns[j].state = USG_SLOT_LIVE;
        atomic_store_explicit(
            &ns[j].count,
            atomic_load_explicit(&u->slots[i].count, memory_order_relaxed),
            memory_order_relaxed);
        atomic_store_explicit(
            &ns[j].last_ms,
            atomic_load_explicit(&u->slots[i].last_ms, memory_order_relaxed),
            memory_order_relaxed);
    }
    free(u->slots);
    u->slots = ns;
    u->cap = ncap;
    u->used = u->live;
    return 0;
}

UsageIndex *usage_index_create(void) { return calloc(1, sizeof(UsageIndex)); }

void usage_index_free(UsageIndex *u) {
    if (!u) {
        return;
    }
    free(u->slots);
    free(u);
}

int usage_index_track(UsageIndex *u, uint64_t id) {
    if (!u) {
        return 0; /* not configured: nothing to do, not a failure */
    }
    if (slot_of(u, id)) {
        return 0; /* already tracked; keep its counters */
    }
    if (!u->cap || ((u->used + 1) * 4) >= (u->cap * 3)) {
        if (grow(u) != 0) {
            return -1;
        }
    }
    size_t mask = u->cap - 1;
    size_t i = (size_t)(mix64(id) & mask);
    UsageEntry *reuse = NULL;
    for (size_t probe = 0; probe <= mask; probe++) {
        UsageEntry *e = &u->slots[(i + probe) & mask];
        if (e->state == USG_SLOT_DEAD && !reuse) {
            reuse = e;
        }
        if (e->state == USG_SLOT_EMPTY) {
            UsageEntry *s = reuse ? reuse : e;
            if (!reuse) {
                u->used++;
            }
            s->id = id;
            s->state = USG_SLOT_LIVE;
            atomic_store_explicit(&s->count, 0, memory_order_relaxed);
            atomic_store_explicit(&s->last_ms, 0, memory_order_relaxed);
            u->live++;
            return 0;
        }
    }
    return -1; /* unreachable at a 0.75 load factor */
}

void usage_index_untrack(UsageIndex *u, uint64_t id) {
    UsageEntry *e = slot_of(u, id);
    if (!e) {
        return;
    }
    e->state = USG_SLOT_DEAD;
    u->live--;
}

void usage_index_record(const UsageIndex *u, uint64_t id, uint64_t now_ms) {
    UsageEntry *e = slot_of(u, id);
    if (!e) {
        return; /* untracked (or --no-usage-feedback): nothing to record */
    }
    /* Relaxed is right: these are a heuristic with no ordering requirement
     * against anything else, and a lost update under contention costs one
     * observation out of many. */
    atomic_fetch_add_explicit(&e->count, 1, memory_order_relaxed);
    atomic_store_explicit(&e->last_ms, now_ms, memory_order_relaxed);
}

int usage_index_get(const UsageIndex *u, uint64_t id, uint32_t *out_count,
                    uint64_t *out_last_ms) {
    const UsageEntry *e = slot_of(u, id);
    if (!e) {
        return -1;
    }
    if (out_count) {
        *out_count =
            (uint32_t)atomic_load_explicit(&e->count, memory_order_relaxed);
    }
    if (out_last_ms) {
        *out_last_ms =
            (uint64_t)atomic_load_explicit(&e->last_ms, memory_order_relaxed);
    }
    return 0;
}

size_t usage_index_count(const UsageIndex *u) { return u ? u->live : 0; }

size_t usage_index_bytes(const UsageIndex *u) {
    if (!u) {
        return 0;
    }
    return sizeof(*u) + (u->cap * sizeof(UsageEntry));
}

uint64_t usage_index_total_recalls(const UsageIndex *u) {
    if (!u) {
        return 0;
    }
    uint64_t total = 0;
    for (size_t i = 0; i < u->cap; i++) {
        if (u->slots[i].state == USG_SLOT_LIVE) {
            total += (uint64_t)atomic_load_explicit(&u->slots[i].count,
                                                    memory_order_relaxed);
        }
    }
    return total;
}

uint8_t *usage_index_serialize(const UsageIndex *u, size_t *out_len) {
    *out_len = 0;
    if (!u) {
        return NULL;
    }
    /* Only records with a recall are worth persisting: a zeroed slot carries no
     * information and the write path recreates it at recovery anyway. */
    size_t n = 0;
    for (size_t i = 0; i < u->cap; i++) {
        if (u->slots[i].state == USG_SLOT_LIVE &&
            atomic_load_explicit(&u->slots[i].count, memory_order_relaxed)) {
            n++;
        }
    }
    size_t len = USG_HDR + (n * USG_ENTRY);
    uint8_t *buf = malloc(len);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, USG_MAGIC, 4);
    aegis_put_u32le(buf + 4, USG_VERSION);
    aegis_put_u64le(buf + 8, (uint64_t)n);
    uint8_t *p = buf + USG_HDR;
    for (size_t i = 0; i < u->cap && (size_t)(p - buf) < len; i++) {
        if (u->slots[i].state != USG_SLOT_LIVE) {
            continue;
        }
        uint32_t c = (uint32_t)atomic_load_explicit(&u->slots[i].count,
                                                    memory_order_relaxed);
        if (!c) {
            continue;
        }
        aegis_put_u64le(p, u->slots[i].id);
        aegis_put_u32le(p + 8, c);
        aegis_put_u64le(p + 12,
                        (uint64_t)atomic_load_explicit(&u->slots[i].last_ms,
                                                       memory_order_relaxed));
        p += USG_ENTRY;
    }
    *out_len = len;
    return buf;
}

int usage_index_load_buf(UsageIndex *u, const uint8_t *buf, size_t len) {
    if (!u || !buf || len < USG_HDR) {
        return -1;
    }
    if (memcmp(buf, USG_MAGIC, 4) != 0 ||
        aegis_get_u32le(buf + 4) != USG_VERSION) {
        return -1;
    }
    uint64_t n = aegis_get_u64le(buf + 8);
    if (n > (len - USG_HDR) / USG_ENTRY) {
        return -1; /* truncated image */
    }
    const uint8_t *p = buf + USG_HDR;
    for (uint64_t i = 0; i < n; i++, p += USG_ENTRY) {
        uint64_t id = aegis_get_u64le(p);
        /* Restore only onto a slot the log already established as live: an id
         * deleted since the checkpoint must not come back to life here. */
        UsageEntry *e = slot_of(u, id);
        if (!e) {
            continue;
        }
        atomic_store_explicit(&e->count, aegis_get_u32le(p + 8),
                              memory_order_relaxed);
        atomic_store_explicit(&e->last_ms, aegis_get_u64le(p + 12),
                              memory_order_relaxed);
    }
    return 0;
}