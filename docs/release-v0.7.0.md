# AegisDB v0.7.0 — forgetting informed by what you actually use 🧠

**`forget` now scores memories on evidence instead of a guess.** Every record
carries a recall count and a last-recalled time, and retention weighs them: a
fact recalled yesterday survives even if it was written a year ago, and one
nothing ever retrieves ages out.

Minor bump — additive on the wire, but there is a new on-disk file, a new config
flag, and `forget` deletes a different set of records than it used to. No
migration; upgrade in place.

## The problem

`forget` scored `importance × recency`, where `importance` is a number set once at
write time — by a caller, or by an extractor's guess — and never revisited.
Meanwhile the strongest signal available was being discarded on every single
request: **which memories retrieval actually surfaces.**

That gets both halves wrong for a long-running corpus. A convention recalled
weekly for a year looks stale, because "recency" meant the last *write*. A
verbose note nobody has ever read looks valuable, because someone typed `0.9`
into `importance` once.

## What changed

```
retention = importance × 0.5^(age / half_life) × use_boost

age        from the last recall, or `updated` if never recalled
use_boost  1 + usage_weight × (1 − 1 / (1 + recall_count))     (saturating)
```

Records returned by `get` and `search` now include:

```json
{ "id": 42, "data": "…", "recall_count": 17, "last_recalled": 1785698888518 }
```

`recall_count: 0` means tracked but never recalled. The fields are **absent**
entirely when the server keeps no counters, so the two cases stay
distinguishable.

## What counts as a recall

Deliberately narrow, because a usage signal that is easy to inflate is worthless:

- **`search` counts the records it returns.** Being a candidate is not being used.
- **`get` reports the counters without incrementing them.** Fetching a known id
  is not retrieval; counting it would let any tool that walks ids inflate every
  record.
- **`"track_usage": false`** on a `search` opts out. The bundled inspector sends
  it, so browsing your memories does not protect them from `forget` — otherwise
  looking at the data would change it.

## Operator notes

- **New file: `usage.db`** in the data directory. Every other index is derived
  from the log and rebuilt at startup; this one records what retrieval surfaced,
  which the log never contained, so it is checkpointed (encrypted with the log
  when encryption is on). Losing it costs `forget` its ranking signal, never data
  — a fresh server simply starts with no history.
- **`forget` will delete a different set than before.** Run it with
  `"dry_run": true` once after upgrading to see the new outcome, and compare
  against `"usage_weight": 0`, which reproduces the old scoring exactly.
- **`usage_weight`** (default `1.0`) tunes how much history protects a record: a
  well-used one can be worth at most twice an equivalent unused one. `0` disables
  usage weighting entirely. Negative is rejected.
- **`--no-usage-feedback`** turns the whole thing off: no counters, no checkpoint,
  no retention boost, and `stats` reports `usage_tracked: 0` / `usage_bytes: 0`.
- **Index RAM grows.** An entry is 32 bytes, but the table is open-addressed and
  kept under a 0.75 load factor, so budget **~40–85 bytes per live record**
  depending on where it sits between doublings (measured: 82 B/record at 100
  records, 66 at 1k, 52 at 5k). `stats` reports `indexes.usage_tracked` and
  `memory.usage_bytes`, and it counts toward `--max-index-bytes` like every other
  index.
- **The read path stays cheap.** Counters are atomics bumped under the index
  *read* lock; the table's structure only changes on the write path. A recall
  performs no allocation and no lock upgrade. Verified under ThreadSanitizer.

## Known limitation

The recall count itself does not decay. A burst of recalls a year ago counts as
much as steady use today; `last_recalled` driving the recency term is what limits
the damage. If this misbehaves on a real corpus, a decayed count is the fix — and
`usage_weight: 0` is the immediate escape hatch.

## Links

- Wire protocol: [wire-protocol.md](wire-protocol.md) — `forget`, the record
  fields, `track_usage`
- Architecture: [architecture.md](architecture.md#usage-feedback) — why this is
  the one index the log cannot rebuild
- Roadmap: [ROADMAP.md](ROADMAP.md) — Horizon 2.3

*No migration required. MIT licensed. Built in C17, no runtime dependencies.*