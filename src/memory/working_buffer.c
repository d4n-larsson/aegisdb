/* Working memory ring buffer with TTL (T041, T042). */
#include "aegisdb/working_buffer.h"

#include <pthread.h>
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
    /* The working store is mutated concurrently by every io-thread (each handles
     * MEM_WORKING inserts/promotes off the index_lock) and by the maintenance
     * thread's sweep. A single store-wide mutex serialises all public entry
     * points; the internal helpers (get_session/find_slot) run under it. */
    pthread_mutex_t lock;
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
    if (pthread_mutex_init(&ws->lock, NULL) != 0) {
        free(ws);
        return NULL;
    }
    return ws;
}

size_t working_store_count(const WorkingStore *ws) {
    if (!ws) return 0;
    WorkingStore *w = (WorkingStore *)ws; /* mutex use requires non-const */
    pthread_mutex_lock(&w->lock);
    size_t n = 0;
    for (size_t i = 0; i < SBUCKETS; i++)
        for (const Session *s = w->buckets[i]; s; s = s->next)
            for (uint32_t k = 0; k < s->capacity; k++)
                if (s->slots[k].rec) n++;
    pthread_mutex_unlock(&w->lock);
    return n;
}

void working_store_free(WorkingStore *ws) {
    if (!ws) return;
    pthread_mutex_destroy(&ws->lock);
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
    pthread_mutex_lock(&ws->lock);
    Session *s = get_session(ws, session_id, 1);
    if (!s) {
        pthread_mutex_unlock(&ws->lock);
        return -1;
    }
    MemoryRecord *copy = record_clone(rec);
    if (!copy) {
        pthread_mutex_unlock(&ws->lock);
        return -1;
    }
    copy->type = MEM_WORKING;
    copy->id = s->next_id++;
    copy->created = now;
    copy->updated = now;
    uint64_t ttl = ttl_ms ? ttl_ms : ws->default_ttl_ms;
    /* Saturate rather than wrap: a huge ttl must read as "far future", not fold
     * now+ttl past the epoch and make the entry look already-expired (matching
     * the persisted-insert path in qe_insert). */
    copy->expires_at = ttl > UINT64_MAX - now ? UINT64_MAX : now + ttl;

    Slot *slot = &s->slots[s->head];
    if (slot->rec) { /* evict oldest */
        record_free(slot->rec);
        free(slot->rec);
    }
    slot->rec = copy;
    s->head = (s->head + 1) % s->capacity;
    if (out_id) *out_id = copy->id;
    pthread_mutex_unlock(&ws->lock);
    return 0;
}

MemoryRecord *working_store_get(WorkingStore *ws, const char *session_id,
                                uint64_t id, uint64_t now) {
    pthread_mutex_lock(&ws->lock);
    Session *s = get_session(ws, session_id, 0);
    MemoryRecord *out = NULL;
    if (s) {
        Slot *sl = find_slot(s, id, now);
        if (sl) out = record_clone(sl->rec);
    }
    pthread_mutex_unlock(&ws->lock);
    return out;
}

int working_store_take(WorkingStore *ws, const char *session_id, uint64_t id,
                       uint64_t now, const char *ns, MemoryRecord *out) {
    pthread_mutex_lock(&ws->lock);
    Session *s = get_session(ws, session_id, 0);
    if (!s) {
        pthread_mutex_unlock(&ws->lock);
        return -1;
    }
    Slot *sl = find_slot(s, id, now);
    if (!sl) {
        pthread_mutex_unlock(&ws->lock);
        return -1;
    }
    /* Tenant ownership: a namespaced caller may only take its own record, so a
     * known/guessed session_id can't promote another tenant's working memory. */
    if (ns && (!sl->rec->agent_id || strcmp(sl->rec->agent_id, ns) != 0)) {
        pthread_mutex_unlock(&ws->lock);
        return -1;
    }
    *out = *sl->rec; /* move ownership */
    free(sl->rec);
    sl->rec = NULL;
    pthread_mutex_unlock(&ws->lock);
    return 0;
}

size_t working_store_sweep(WorkingStore *ws, uint64_t now) {
    size_t removed = 0;
    pthread_mutex_lock(&ws->lock);
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
    pthread_mutex_unlock(&ws->lock);
    return removed;
}