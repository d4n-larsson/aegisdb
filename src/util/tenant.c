/* Per-tenant usage accounting + token-bucket rate limiting (see tenant.h).
 *
 * Tenants are few (team-scale), so the table is a plain dynamic array scanned
 * linearly under one mutex — simpler than a hash map and fast at this size. */
#include "aegisdb/tenant.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *ns;
    uint64_t records; /* live records */
    uint64_t bytes;   /* live bytes (sum of frame payload lengths) */
    /* token bucket */
    double tokens;
    uint64_t last_refill_us;
} Tenant;

struct TenantTable {
    pthread_mutex_t lock;
    Tenant *t;
    size_t n, cap;
};

TenantTable *tenant_table_create(void) {
    TenantTable *tt = calloc(1, sizeof(*tt));
    if (!tt) return NULL;
    if (pthread_mutex_init(&tt->lock, NULL) != 0) {
        free(tt);
        return NULL;
    }
    return tt;
}

void tenant_table_free(TenantTable *tt) {
    if (!tt) return;
    for (size_t i = 0; i < tt->n; i++) free(tt->t[i].ns);
    free(tt->t);
    pthread_mutex_destroy(&tt->lock);
    free(tt);
}

/* Find `ns`, or append a zeroed entry for it. Caller holds the lock. Returns
 * NULL only on allocation failure. */
static Tenant *find_or_add(TenantTable *tt, const char *ns) {
    for (size_t i = 0; i < tt->n; i++)
        if (strcmp(tt->t[i].ns, ns) == 0) return &tt->t[i];
    if (tt->n == tt->cap) {
        size_t nc = tt->cap ? tt->cap * 2 : 8;
        Tenant *g = realloc(tt->t, nc * sizeof(*g));
        if (!g) return NULL;
        tt->t = g;
        tt->cap = nc;
    }
    Tenant *e = &tt->t[tt->n];
    memset(e, 0, sizeof(*e));
    e->ns = strdup(ns);
    if (!e->ns) return NULL;
    tt->n++;
    return e;
}

static Tenant *find(TenantTable *tt, const char *ns) {
    for (size_t i = 0; i < tt->n; i++)
        if (strcmp(tt->t[i].ns, ns) == 0) return &tt->t[i];
    return NULL;
}

void tenant_usage_adjust(TenantTable *tt, const char *ns, long d_records,
                         long d_bytes) {
    if (!tt || !ns) return;
    pthread_mutex_lock(&tt->lock);
    Tenant *e = find_or_add(tt, ns);
    if (e) {
        /* Saturate at 0 rather than underflow (defends against any accounting
         * skew; usage is a guard rail, not ledger-exact). */
        if (d_records < 0 && (uint64_t)(-d_records) > e->records) e->records = 0;
        else e->records = (uint64_t)((long)e->records + d_records);
        if (d_bytes < 0 && (uint64_t)(-d_bytes) > e->bytes) e->bytes = 0;
        else e->bytes = (uint64_t)((long)e->bytes + d_bytes);
    }
    pthread_mutex_unlock(&tt->lock);
}

int tenant_usage_would_exceed(TenantTable *tt, const char *ns, long add_records,
                              long add_bytes, size_t max_records,
                              size_t max_bytes) {
    if (!tt || !ns || (max_records == 0 && max_bytes == 0)) return 0;
    int exceed = 0;
    pthread_mutex_lock(&tt->lock);
    const Tenant *e = find(tt, ns);
    uint64_t recs = e ? e->records : 0;
    uint64_t bytes = e ? e->bytes : 0;
    if (max_records && add_records > 0 &&
        recs + (uint64_t)add_records > max_records)
        exceed = 1;
    if (max_bytes && add_bytes > 0 && bytes + (uint64_t)add_bytes > max_bytes)
        exceed = 1;
    pthread_mutex_unlock(&tt->lock);
    return exceed ? -1 : 0;
}

int tenant_rate_allow(TenantTable *tt, const char *ns, double qps, double burst,
                      uint64_t now_us) {
    if (!tt || !ns || qps <= 0) return 1; /* no limit */
    if (burst < 1.0) burst = 1.0;
    int allow;
    pthread_mutex_lock(&tt->lock);
    Tenant *e = find_or_add(tt, ns);
    if (!e) {
        pthread_mutex_unlock(&tt->lock);
        return 1; /* fail open on OOM: availability over strict limiting */
    }
    if (e->last_refill_us == 0) {
        e->tokens = burst; /* first sight: full bucket */
        e->last_refill_us = now_us;
    } else if (now_us > e->last_refill_us) {
        double elapsed_s = (double)(now_us - e->last_refill_us) / 1e6;
        e->tokens += elapsed_s * qps;
        if (e->tokens > burst) e->tokens = burst;
        e->last_refill_us = now_us;
    }
    if (e->tokens >= 1.0) {
        e->tokens -= 1.0;
        allow = 1;
    } else {
        allow = 0;
    }
    pthread_mutex_unlock(&tt->lock);
    return allow;
}

TenantUsage *tenant_usage_snapshot(TenantTable *tt, size_t *out_n) {
    if (out_n) *out_n = 0;
    if (!tt) return NULL;
    pthread_mutex_lock(&tt->lock);
    TenantUsage *out = tt->n ? calloc(tt->n, sizeof(*out)) : NULL;
    size_t got = 0;
    if (out) {
        for (size_t i = 0; i < tt->n; i++) {
            out[i].ns = strdup(tt->t[i].ns);
            if (!out[i].ns) break; /* partial on OOM; freed below via got */
            out[i].records = tt->t[i].records;
            out[i].bytes = tt->t[i].bytes;
            got++;
        }
    }
    pthread_mutex_unlock(&tt->lock);
    if (out && got != tt->n) { /* OOM mid-copy: return the prefix we secured */
        /* (got may be < n; caller frees `got` entries) */
    }
    if (out_n) *out_n = got;
    if (out && got == 0) { free(out); return NULL; }
    return out;
}

void tenant_usage_free(TenantUsage *u, size_t n) {
    if (!u) return;
    for (size_t i = 0; i < n; i++) free(u[i].ns);
    free(u);
}