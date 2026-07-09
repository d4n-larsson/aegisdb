/* Per-tenant (namespace) resource accounting: storage usage + request rate.
 *
 * On a shared multi-tenant server, quotas and rate limits stop one tenant's
 * runaway agent from filling the disk or monopolizing the server and degrading
 * everyone else. A tenant is a namespace (the `agent_id` a namespaced token is
 * bound to); accounting is keyed by that string. Enforcement is opt-in per
 * limit (0 = unlimited) and only meaningful when auth is enabled — with auth
 * off there are no namespaces to isolate.
 *
 * The table is self-synchronized (its own mutex), so callers need not hold any
 * other lock. Keep it a leaf: never take another lock while inside these calls.
 */
#ifndef AEGISDB_TENANT_H
#define AEGISDB_TENANT_H

#include <stddef.h>
#include <stdint.h>

typedef struct TenantTable TenantTable;

TenantTable *tenant_table_create(void);
void tenant_table_free(TenantTable *t);

/* Adjust a namespace's live-record and live-byte usage by signed deltas (called
 * as records enter/leave the live set). Creates the tenant entry on first use. */
void tenant_usage_adjust(TenantTable *t, const char *ns, long d_records,
                         long d_bytes);

/* Would adding `add_records`/`add_bytes` to `ns` exceed a configured limit?
 * A limit of 0 means unlimited. Returns 0 if within limits, -1 if it would
 * exceed either. Does not mutate. */
int tenant_usage_would_exceed(TenantTable *t, const char *ns, long add_records,
                              long add_bytes, size_t max_records,
                              size_t max_bytes);

/* Token-bucket rate check for `ns`: refills `qps` tokens/second up to `burst`,
 * consumes one token. Returns 1 if allowed, 0 if the tenant is over its rate.
 * `now_us` is a CLOCK_MONOTONIC timestamp in microseconds. qps <= 0 = no limit. */
int tenant_rate_allow(TenantTable *t, const char *ns, double qps, double burst,
                      uint64_t now_us);

/* A copied-out view of one tenant's usage (owns its `ns` string). */
typedef struct {
    char *ns;
    uint64_t records;
    uint64_t bytes;
} TenantUsage;

/* Snapshot every tenant's usage into a freshly allocated array (copied under
 * the lock, so it stays valid regardless of concurrent mutation). Sets *out_n;
 * returns NULL when there are no tenants or on allocation failure. Free with
 * tenant_usage_free. */
TenantUsage *tenant_usage_snapshot(TenantTable *t, size_t *out_n);
void tenant_usage_free(TenantUsage *u, size_t n);

#endif /* AEGISDB_TENANT_H */