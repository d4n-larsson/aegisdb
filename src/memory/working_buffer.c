/* Working memory ring buffer with TTL (T041, T042). */
#include "aegisdb/working_buffer.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    MemoryRecord *rec; /* owned; NULL = empty slot */
} Slot;

typedef struct Session {
    char *session_id;
    Slot *slots;       /* ring buffer of capacity entries */
    uint32_t capacity;
    uint32_t head;     /* next write position */
    uint64_t next_id;  /* session-local id counter */
    struct Session *next;
} Session;

#define SBUCKETS 256

struct WorkingStore {
    Session *buckets[SBUCKETS];
    uint32_t capacity;
    uint64_t default_ttl_ms;
};

static size_t hash_str(const char *s) {
    size_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h % SBUCKETS;
}

WorkingStore *working_store_create(uint32_t capacity, uint64_t default_ttl_ms) {
    WorkingStore *ws = calloc(1, sizeof(*ws));
    if (!ws) return NULL;
    ws->capacity = capacity ? capacity : 256;
    ws->default_ttl_ms = default_ttl_ms ? default_ttl_ms : 3600000;
    return ws;
}

size_t working_store_count(const WorkingStore *ws) {
    if (!ws) return 0;
    size_t n = 0;
    for (size_t i = 0; i < SBUCKETS; i++)
        for (const Session *s = ws->buckets[i]; s; s = s->next)
            for (uint32_t k = 0; k < s->capacity; k++)
                if (s->slots[k].rec) n++;
    return n;
}

void working_store_free(WorkingStore *ws) {
    if (!ws) return;
    for (size_t i = 0; i < SBUCKETS; i++) {
        Session *s = ws->buckets[i];
        while (s) {
            Session *nx = s->next;
            for (uint32_t k = 0; k < s->capacity; k++) {
                if (s->slots[k].rec) {
                    record_free(s->slots[k].rec);
                    free(s->slots[k].rec);
                }
            }
            free(s->slots);
            free(s->session_id);
            free(s);
            s = nx;
        }
    }
    free(ws);
}

static Session *get_session(WorkingStore *ws, const char *sid, int create) {
    size_t b = hash_str(sid);
    for (Session *s = ws->buckets[b]; s; s = s->next)
        if (strcmp(s->session_id, sid) == 0) return s;
    if (!create) return NULL;
    Session *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->session_id = strdup(sid);
    s->capacity = ws->capacity;
    s->slots = calloc(s->capacity, sizeof(Slot));
    s->next_id = 1;
    if (!s->session_id || !s->slots) {
        free(s->session_id);
        free(s->slots);
        free(s);
        return NULL;
    }
    s->next = ws->buckets[b];
    ws->buckets[b] = s;
    return s;
}

static Slot *find_slot(Session *s, uint64_t id, uint64_t now) {
    for (uint32_t k = 0; k < s->capacity; k++) {
        Slot *sl = &s->slots[k];
        if (!sl->rec) continue;
        if (sl->rec->expires_at && now >= sl->rec->expires_at) {
            record_free(sl->rec);
            free(sl->rec);
            sl->rec = NULL;
            continue;
        }
        if (sl->rec->id == id) return sl;
    }
    return NULL;
}

int working_store_add(WorkingStore *ws, const char *session_id,
                      const MemoryRecord *rec, uint64_t now, uint64_t ttl_ms,
                      uint64_t *out_id) {
    Session *s = get_session(ws, session_id, 1);
    if (!s) return -1;
    MemoryRecord *copy = record_clone(rec);
    if (!copy) return -1;
    copy->type = MEM_WORKING;
    copy->id = s->next_id++;
    copy->created = now;
    copy->updated = now;
    uint64_t ttl = ttl_ms ? ttl_ms : ws->default_ttl_ms;
    copy->expires_at = now + ttl;

    Slot *slot = &s->slots[s->head];
    if (slot->rec) { /* evict oldest */
        record_free(slot->rec);
        free(slot->rec);
    }
    slot->rec = copy;
    s->head = (s->head + 1) % s->capacity;
    if (out_id) *out_id = copy->id;
    return 0;
}

MemoryRecord *working_store_get(WorkingStore *ws, const char *session_id,
                                uint64_t id, uint64_t now) {
    Session *s = get_session(ws, session_id, 0);
    if (!s) return NULL;
    Slot *sl = find_slot(s, id, now);
    if (!sl) return NULL;
    return record_clone(sl->rec);
}

int working_store_take(WorkingStore *ws, const char *session_id, uint64_t id,
                       uint64_t now, const char *ns, MemoryRecord *out) {
    Session *s = get_session(ws, session_id, 0);
    if (!s) return -1;
    Slot *sl = find_slot(s, id, now);
    if (!sl) return -1;
    /* Tenant ownership: a namespaced caller may only take its own record, so a
     * known/guessed session_id can't promote another tenant's working memory. */
    if (ns && (!sl->rec->agent_id || strcmp(sl->rec->agent_id, ns) != 0))
        return -1;
    *out = *sl->rec; /* move ownership */
    free(sl->rec);
    sl->rec = NULL;
    return 0;
}

size_t working_store_sweep(WorkingStore *ws, uint64_t now) {
    size_t removed = 0;
    for (size_t i = 0; i < SBUCKETS; i++) {
        for (Session *s = ws->buckets[i]; s; s = s->next) {
            for (uint32_t k = 0; k < s->capacity; k++) {
                Slot *sl = &s->slots[k];
                if (sl->rec && sl->rec->expires_at &&
                    now >= sl->rec->expires_at) {
                    record_free(sl->rec);
                    free(sl->rec);
                    sl->rec = NULL;
                    removed++;
                }
            }
        }
    }
    return removed;
}