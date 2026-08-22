# AegisDB v0.6.1 — you can now see recall latency 📈

A small observability release, and a direct follow-up to v0.6.0: that release put
a new index on the recall path and told operators to watch its cost, without
giving them an instrument to see it. **`search` latency is now a histogram**, so
p95/p99 are observable instead of averaged away.

Purely additive — no API, wire-protocol, or on-disk format change. Upgrade in
place.

## The gap this closes

The only latency signal was `dispatch_micros`: cumulative in-dispatch time across
*every* operation. As a mean over a mixed workload it tells you almost nothing
about recall specifically, and a mean hides the tail by construction. Recall runs
in the agent's inner loop — it is the one path where the 99th percentile is the
number that matters, because that is the turn the user waits for.

## What you get

`stats` gains `metrics.recall_latency`:

```json
{
  "count": 1240, "micros_total": 297600, "mean_micros": 240.0,
  "p50_micros": 180.0, "p95_micros": 640.0, "p99_micros": 2100.0,
  "buckets": { "100": 210, "250": 900, "500": 1100, "1000": 1180,
               "2500": 1235, "5000": 1240, "…": 1240, "+Inf": 1240 }
}
```

- **12 buckets, 100µs → 250ms** plus an overflow bucket — dense where recall
  should live, sparse past the point where it is already too slow to matter.
- **Cumulative counts** keyed by upper bound in microseconds, i.e. Prometheus `le`
  semantics (each bucket includes every faster one; `+Inf` equals `count`), so a
  scraper can pass them straight through.
- **Percentiles in the response.** They are derivable from the buckets, but most
  people running this do not have Prometheus, and `aegisdb client stats` should be
  able to answer "is recall slow?" on its own. Linearly interpolated within the
  bucket — the same approximation `histogram_quantile` makes.
- **Only `search` is observed.** This is recall latency, not request latency;
  `dispatch_micros` already covers everything else.
- **Absent until the first search.** A fresh server reports no distribution rather
  than an all-zero one, because a server claiming `p99 = 0` is worse than one
  saying nothing.

## Prometheus + Grafana

The exporter emits a real histogram family, in seconds:

```
aegisdb_recall_latency_seconds_bucket{le="0.00025"} 900
aegisdb_recall_latency_seconds_sum   0.2976
aegisdb_recall_latency_seconds_count 1240
```

The alert worth having:

```promql
histogram_quantile(0.99, sum(rate(aegisdb_recall_latency_seconds_bucket[5m])) by (le))
```

The bundled dashboard gains a **Recall latency percentiles (search)** panel
plotting p50/p95/p99 with the mean dashed alongside — so the tail and the average
are visible together, which is the point.

## Operator notes

- **Additive only.** A scraper that reads specific `metrics` keys is unaffected;
  one that assumes a fixed key set should tolerate the new object (the bundled
  exporter already checks for presence).
- **No new configuration.** The histogram is 14 atomic counters and always on;
  there is no flag to enable or size it.
- Metrics remain **cumulative since server start**, like every other counter here
  — take successive diffs for rates, and expect a reset on restart.
- Also corrects a stale roadmap entry: 2.2 listed semantic contradiction detection
  as remaining when 2.1 had already shipped it as `judge_supersedes`.

## Links

- Wire protocol: [wire-protocol.md](wire-protocol.md) — the `stats` response
- Exporter: [prometheus-exporter/README.md](../integrations/prometheus-exporter/README.md)
- Roadmap: [ROADMAP.md](ROADMAP.md) — Horizon 3.3

*No migration required. MIT licensed. Built in C17, no runtime dependencies.*