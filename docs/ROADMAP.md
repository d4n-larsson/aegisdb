# AegisDB Roadmap

> Where the leverage is, and the order to pursue it. This is a product-strategy
> document, not a task list — each horizon states *why now*, *what to build*, and
> *what "done" looks like*, and calls out the primitives already in the tree that
> it builds on.

## North Star

**Make AI agents that remember — and let their operators trust what they
remember.**

Storage is the commodity part of agent memory (pgvector, Redis, Qdrant, Chroma
all store vectors). AegisDB already has a strong storage engine: HNSW + exact
scan, tags, a time index, relationships/traversal, TTL, namespaces/tenancy,
auth, encryption at rest, replication, snapshots, compaction, crash recovery.
The durable, single-binary C core is a genuine advantage over a Python service
bolted onto Postgres.

The **value and the moat are one layer up**: deciding *what* to remember, keeping
it *coherent* over time, and *proving why* a memory surfaced. That is where
mem0 / Zep / Letta compete, and where a plain vector DB cannot follow.

## Strategic thesis

1. **Win a sharp wedge before becoming a platform.** The strongest wedge already
   lives in `integrations/claude-code/`: *persistent memory for coding agents
   across sessions and repos.* It is concrete, demoable, and viral. The general
   agent-memory API follows the wedge — not the other way around.
2. **Compete on the memory-quality layer, not the index.** Extraction,
   deduplication, contradiction resolution, consolidation, decay. This is what a
   user actually feels.
3. **Trust is the adoption blocker.** People don't ship agent memory because they
   can't tell if it's helping or hallucinating context. Provenance, measurable
   recall quality, and an inspect/edit surface unblock adoption.
4. **Operability is a quiet moat — finish the story.** Durability, encryption,
   multi-tenant isolation, backups, and a hosted tier turn the C core into a
   sales line.

## Non-goals

- Becoming a general-purpose vector database. We are an *agent memory* system;
  the index is a means, not the product.
- A bespoke query language. The newline-delimited JSON wire protocol stays small.
- Locking to one model provider. Extraction/distillation stays provider-neutral
  (the summarizer's `none`/`fake`/`claude-code`/`anthropic`/`openai` seam is the
  pattern to follow everywhere).

---

## Horizon 1 — Now: trust the wedge  ✅ *complete*

*Theme: make the coding-agent memory use case demoable and trustworthy end to
end. Nothing here needs a model on the hot path.*

### 1.1 Recall-quality eval harness *(foundational — do first)* — *shipped (`make eval`)*
- **Why now:** we just made *bugs* regression-tested; memory *quality* is
  currently unmeasured, so every change to scoring/recall is a guess. Everything
  downstream (decay, extraction, distillation tuning) needs a scoreboard.
- **Build:** a harness that seeds a corpus, runs a labelled query set, and scores
  whether the right memories surfaced — `recall@k`, `MRR`, and a per-query report.
  Ship a small starter dataset for the coding-agent scenario. Make it runnable in
  CI as a non-blocking report first, then a gate on regressions.
- **Leverages:** `search` (time/tags/embedding), the hybrid scorer
  (importance × confidence × recency), namespaces for isolation.
- **Done when:** `make eval` prints recall@k/MRR against a checked-in dataset, and
  a scoring change moves the numbers visibly.

### 1.2 Provenance on every retrieved memory — *score breakdown shipped*
- **Why now:** trust starts with "where did this come from and when?" Cheap given
  the data already stored.
- **Build:** surface per-hit provenance in `search`/`recall` responses — source,
  `created`/`updated`, supersession chain, and a **score breakdown** (why this
  ranked here: semantic vs. recency vs. importance contribution).
- **Leverages:** existing record metadata, relationships, the semantic
  supersession already in the log.
- **Done when:** every hit can answer "why am I seeing this?" without server-side
  guesswork.

### 1.3 Memory-inspection surface — *shipped (`make inspector`)*
- **Why now:** the single most effective *sales/adoption* artifact — let a human
  see and correct what the agent believes. The repo already has dashboard muscle
  (Grafana).
- **Build:** a browse/search/inspect view over a namespace: list memories, see the
  1.2 score breakdown and provenance per hit, edit/delete. Start as a
  self-contained artifact/dashboard reading the existing `stats`/`search` surface.
- **Done when:** a Claude Code user can watch memory accumulate across sessions
  and hand-correct a wrong memory.

---

## Horizon 2 — Next: memory that stays coherent  ✅ *complete*

*2.1 extraction (+ contradiction → supersession), 2.2 dedup + provenance, and
2.3 decay/forgetting are all shipped and measurable. The server owns the
mechanical primitives (`consolidate`+`supersedes`, `forget`); the integration
owns the model-driven judgment (extraction, contradiction detection, distillation)
behind the provider seam.*

*Theme: the memory-quality layer. Turn existing primitives into real policy.*

### 2.1 Extraction (write-path quality) — *shipped (`extract_mode`)*
- **Why now:** today the caller decides what to insert. Owning "turn this
  conversation into durable facts worth keeping" is the difference between a DB
  and a memory *product* (mem0's core pitch).
- **Build:** an extraction step in the integration (provider-neutral, off the hot
  path) that distils turns into candidate facts with importance/confidence, then
  writes them through the dedup/contradiction policy below.
- **Leverages:** the existing summarizer provider seam
  (`none`/`fake`/`claude-code`/`anthropic`/`openai`) — reuse it, don't reinvent.
- **Shipped:** `aegis_mcp/extract.py` — an `ExtractionProvider` seam
  (`none`/`fake`/`claude-code`/`anthropic`/`openai`). When `extract_mode` is on,
  `run_capture` distils the transcript into durable facts stored as **semantic**
  memories (so they dedup/supersede via 2.2 and resist decay via 2.3) instead of
  raw marker-matched sentences; ephemeral content is dropped before the model
  sees it; the heuristic marker path is the `none` fallback. Robust JSON parsing,
  bounded input, best-effort (never breaks capture).
- **Shipped (contradiction → supersession):** on capture, each extracted fact
  recalls similar existing memories and the extractor judges which it makes
  obsolete (`judge_supersedes`); those are tombstoned with a `supersedes`
  provenance link (via `insert` + `relate` + `delete`) instead of accumulating a
  contradiction. Gated by `extract_supersede` (needs embeddings + a backend);
  the deterministic `fake` backend makes it testable. This closes the semantic
  contradiction thread deferred from 2.2.

### 2.2 Dedup + contradiction resolution — *provenance + measurement shipped*
- **Why now:** unbounded, self-contradicting memory is worse than none.
- **Build:** a policy layer over the mechanical `consolidate`:
  near-duplicate collapse (already have cosine dedup), plus **supersession** when
  a new fact contradicts an old one ("prefers X, *not* Y"), recording the
  supersession chain for provenance (1.2).
- **Leverages:** `consolidate`, `MEM_SEMANTIC` supersession in the append-only
  log, `update`/`delete`.
- **Shipped:** consolidation now records a `supersedes` link (survivor → each
  absorbed record) so a merge is auditable lineage, not silent loss (surfaced by
  the inspector + `get`/`search` `relationships`). Made **measurable** with
  `make eval EVAL_ARGS='--consolidate'`: seed duplicate clusters, assert the
  corpus shrinks without losing recall (starter run: 66 → 22 records, recall@10
  held at 93%).
- **Closed:** *semantic* contradiction detection ("prefers X, not Y" — vector-
  similar but opposite) was the open thread here. It shipped in 2.1 as
  `judge_supersedes` in the integration's write path, behind the provider seam,
  calling this server-side supersession mechanism — see the 2.1 entry above.

### 2.3 Decay & forgetting policy — *shipped (`forget` op)*
- **Why now:** recall is injected every turn — its size is a recurring token cost,
  and indexes are in RAM. Forgetting is a feature.
- **Build:** importance × recency decay with configurable half-life; promote
  durable facts, let low-value episodic events age out. Complements the
  already-implemented **LLM distillation** (`summarization-design.md`) which folds
  related aging clusters into one dense fact.
- **Leverages:** TTL sweep, `promote`, the background summarizer.
- **Shipped:** a `forget` maintenance op tombstones records whose
  `retention = importance × 0.5^(age/half_life)` falls below `min_retention`.
  Defaults to episodic-only (curated semantic facts protected), supports
  `dry_run` and a `max_forget` cap, namespaced. Made **measurable** with
  `make eval EVAL_ARGS='--decay'`: seed facts + low-value episodic noise, assert
  the corpus plateaus without losing recall (starter run: 222 → 22 records,
  recall@10 held at 93%).
- **Done when:** a long-running agent's recall size and index RAM plateau instead
  of growing linearly, with recall@k held (measured by 1.1). ✅
- **Shipped (usage feedback):** the policy scored on `importance` — a number the
  writer guessed once and never revisited — while discarding the strongest signal
  available: what retrieval actually surfaces. Every record now carries a recall
  count and a last-recalled time (`usage_index.h/.c`), and `forget` measures
  recency from the last *use* rather than the last write, plus a saturating
  boost of up to `1 + usage_weight` for frequently-recalled records.
  `usage_weight: 0` reproduces the old scoring exactly. Counters are atomics
  bumped under the index *read* lock, so recall stays allocation- and
  lock-upgrade-free; the table's structure only changes on the write path. This
  is the one index the log cannot rebuild, so it checkpoints to `usage.db`.
  `--no-usage-feedback` opts out. `get` reports the counters without
  incrementing them, and a browsing client (the inspector) passes
  `track_usage: false`, so looking at memories does not protect them.
- **Deferred:** model-driven "is this still relevant?" judgment (beyond the
  mechanical importance×recency×usage policy) belongs in a maintenance job on
  top, alongside the LLM distiller.

---

## Horizon 3 — Later: enterprise trust & scale

*Theme: the capabilities that turn "interesting" into "we can deploy this."*

### 3.1 Temporal / bitemporal queries — *shipped (`history` / `get?as_of`)*
- **Why now:** "what did the agent know at time T?" is a real differentiator (part
  of Zep's pitch) and enterprises pay for it. The append-only log already contains
  the history.
- **Shipped:** `history` (every version of an id in causal order, each with a
  derived `[valid_from, valid_to)` interval + `deleted` flag) and `get` with
  `as_of` (the version live at a past time), both reconstructed from the log via a
  scan under `log_lock`, namespace-scoped. **Caveat (documented):** history depth
  is bounded by compaction — it reconstructs from the live log, so a full archival
  trail needs a snapshot or deferred compaction. A compaction-immune history log
  would be the next step toward true bitemporality.
- **Build (original):** point-in-time reads over the log's history; validity
  intervals on facts (valid-from/valid-to) so superseded facts remain auditable
  rather than
  overwritten.
- **Leverages:** the append-only log, snapshots, recovery replay.

### 3.2 Right-to-be-forgotten & export — *shipped (`export` / `purge`)*
- **Why now:** table stakes for any B2B memory sale. For a *memory* product,
  "forget everything about user X" and "export what you store about me" are not
  optional.
- **Build:** compliance-grade hard delete scoped by namespace (verified through
  compaction so data actually leaves disk), plus a per-subject export.
- **Leverages:** namespaces/tenancy, compaction, snapshots.
- **Shipped:** `export` (subject's records, id-paginated via `after_id`/`cursor`)
  and `purge` (tombstone every record in a namespace, then compact so the
  payloads leave `memory.log`). Both are subject-scoped — a namespaced token acts
  only on its own tenant, a spoofed `agent_id` is ignored, and a subjectless
  export/purge is refused; `purge` is `rw`-only with a `dry_run` preview.
- **Done when:** a hard-delete provably removes a subject's data from the log and
  all indexes after compaction, with a test that greps the on-disk log. ✅ — the
  contract test asserts the purged plaintext is absent from `memory.log` after
  compaction while a co-tenant's data survives.

### 3.3 Hosted tier & operability — *observability shipped*
- **Why now:** distribution. The C core's durability/encryption/replication become
  a sales line only if it's trivial to run.
- **Build:** one-line deploy (the `docker-compose.yml` is step one), first-class
  metrics (extend the existing Grafana dashboard: recall latency, index RAM,
  eviction/decay rates, distillation lag), SDKs, and framework adapters beyond MCP.
- **Leverages:** replication, snapshots/restore, health/stats endpoints, encryption.
- **Already in place:** one-command deploy (`docker compose up`, with
  `backup`/`summarize`/`metrics`/`monitoring` profiles), a Prometheus exporter,
  and an auto-provisioned Grafana dashboard.
- **Shipped:** made the **memory-quality layer observable** — cumulative outcome
  counters (`memories_merged`/`memories_forgotten`/`memories_purged`) in the stats
  `metrics`, surfaced by the exporter (`aegisdb_memories_*_total`) and a new
  "Memory-quality activity" dashboard panel. So the Horizon-2/3 work (dedup, decay,
  erase) is now visible in production, not just in the eval harness — the roadmap's
  "eviction/decay rates".
- **Shipped (recall latency):** `search` dispatch time is now bucketed into a
  histogram (`metrics.recall_latency` in `stats`, `aegisdb_recall_latency_seconds`
  from the exporter, a p50/p95/p99 dashboard panel). Recall runs in the agent's
  inner loop, so the tail is what matters and `dispatch_micros` — a cumulative
  mean over *all* operations — hid it. `stats` also reports interpolated
  percentiles directly, so an operator without Prometheus gets the number from
  `aegisdb client stats`. Prompted by 4.1: v0.6.0 added a whole new index to the
  recall path and told operators to watch its cost, without giving them an
  instrument to see it.
- **Remaining:** a standalone, pip-installable client SDK and framework adapters
  beyond MCP; distillation-lag metrics.

---

## Horizon 4 — Now: retrieval that finds identifiers, not just topics  ✅ *complete*

*Theme: close the recall gap dense vectors structurally cannot. Horizons 1–2 made
the memory-quality layer coherent and measurable; this is the retrieval side of
the same bet, and it needs no model on the hot path.*

### 4.1 Lexical search + hybrid fusion — *shipped (`search` `query`, BM25 + RRF)*
- **Why now:** `search` has no text matching at all — the filters are time, tags,
  type, and an embedding vector, so a memory cannot be found by the words it
  contains. That is a structural miss for the coding-agent wedge, whose memories
  are dense with rare tokens: `--tenant-max-records`, `hnsw.c:214`,
  `AEGIS_RECALL_TOP_K`, a CRC framing error string, a PR number. Dense embeddings
  are the wrong instrument for exactly those terms — they are averaged away — and
  a deployment running `embedding_mode=none` (the recall hook's own default) has
  *no* content-based retrieval whatsoever, only tags and time. Every downstream
  policy already shipped (extraction, dedup, decay) is tuned against a retrieval
  stage that cannot answer keyword queries.
- **Build:** an inverted token→postings index alongside `tag_index`, BM25 scoring,
  a `query` string field on `search`, and reciprocal-rank fusion when `query` and
  `embedding` are both supplied. `explain` gains the lexical and fused-rank terms
  so hybrid hits stay as inspectable as semantic ones (1.2). Tokenization must
  keep identifier shape — don't split on `_`/`-`/`.`/`:` or case boundaries, or
  the exact terms this exists to retrieve are destroyed before indexing.
- **Leverages:** the existing tag index's postings machinery, the hybrid scorer
  (importance × confidence × recency) for weighting fused results, `explain`,
  and the eval harness as the scoreboard.
- **Falls out for free:** a working search box in the inspector (1.3), and
  "find every memory mentioning X" for audit/`export` workflows.
- **Done when:** an identifier-heavy query set added to the `make eval` dataset
  scores materially better hybrid than semantic-only, and a server with no
  embedding provider configured still answers content queries.
- **Shipped:** `lexical_index.h/.c` — an inverted term→postings index with the
  per-document stats BM25 needs, plus an identifier-preserving tokenizer (`_ - .
  : / + #` and non-ASCII stay inside a term; a compound also yields its parts, so
  `--tenant-max-records` is findable whole *and* by `records`). `search` takes a
  `query` string; with `embedding` too, the two ranked lists fuse by reciprocal
  rank (`RRF_K = 60`). `explain` gains `lexical`/`bm25`/`semantic_rank`/
  `lexical_rank`/`rrf`, reported **per hit**, so a one-sided match is visible
  rather than inferred. Derived and never checkpointed: recovery rebuilds it from
  the log like time/tag. `--no-lexical-index` opts out (a `query` then gets
  `NOT_READY`); `stats` reports `lexical_terms`/`lexical_docs`/`lexical_bytes` so
  its RAM is watchable like every other index.
- **Measured:** `make eval EVAL_ARGS='--lexical --dataset
  eval/datasets/identifiers.json'` compares semantic-only / lexical-only / hybrid
  over one corpus and gates on *both* aggregate recall and per-query regressions.
  It paid for itself immediately by catching the fused ranking being dominated by
  the `importance × confidence` multiplier (recall@1 62% vs 92% semantic-only):
  RRF scores differ by under 2% between adjacent ranks, so any wider multiplier
  becomes the primary sort key. Hybrid now ranks on the fusion alone (92% → 96%
  recall@1, MRR 0.788 → 0.979).
- **Caveat on the numbers:** the eval's default `hashing` embedder is itself
  token-based, so it behaves like a lexical matcher and scores far better on
  identifier queries (92% recall@1) than a real dense model would. With that
  embedder the comparison is a **regression gate, not a demonstration of the
  gap** — sizing the real gap needs `--embedder command` against an actual model.
  The unambiguous win is structural and already covered by a contract test: a
  record with no embedding at all is retrievable by its terms, which an
  embeddings-only server cannot do.
- **Deferred:** fusing *weighted* per-source ranks (so importance and recency
  shape a hybrid query without swamping it) needs the records loaded before
  fusion rather than after — a real restructure of the candidate path, and the
  reason hybrid currently reports `weight`/`recency_factor` as `1.0`.

---

## Sequencing rationale

```
1.1 eval harness ─┬─> everything downstream is measurable
1.2 provenance ───┼─> feeds 1.3 inspection UI
1.3 inspection ───┘
        │
2.1 extraction ──> 2.2 dedup/contradiction ──> 2.3 decay/forgetting
        (each validated against 1.1)
        │
3.1 temporal ─ 3.2 forget/export ─ 3.3 hosted tier
        │
4.1 lexical + hybrid retrieval  ✅
        (validated against 1.1; graded by the same recall@k/MRR)
```

Build the **scoreboard (1.1)** before the memory-policy work in Horizon 2, so
extraction, dedup, and decay are tuned against numbers rather than vibes. Ship
**provenance + inspection (1.2/1.3)** early because trust — not features — is what
gates adoption of the wedge.

**Lexical (4.1) comes after Horizon 2, not before it** — the policy layer is what
differentiates, and it needed the scoreboard first. But it lands ahead of 3.3's
remaining SDK/metrics work: a keyword query that returns nothing is a recall
failure users feel on every session, whereas a missing SDK is friction for
adopters who haven't arrived yet.

## Risks & how the roadmap answers them

- **Commoditization from above** (model providers ship native memory, frameworks
  bundle it). *Answer:* be better and *inspectable* at the policy + trust layer
  (Horizons 1–2), and stay provider-neutral.
- **Scope drift into a generic vector DB.** *Answer:* the non-goals, and keeping
  every workstream tied to the coding-agent wedge until it's won. Note that 4.1
  is *not* an exception: hybrid retrieval is one `query` field on the existing
  `search` op — not a query language, and not full-text search as a product —
  scoped to a recall failure the wedge hits daily.
- **Polishing the engine because it's concrete** while the unglamorous
  extraction/eval/UI work — where the users are — waits. *Answer:* Horizon 1 is
  deliberately the eval + trust surface, not more storage features.

## The one-line bet

Go deep on **coding-agent memory + a memory-inspection/eval surface**: let a
Claude Code user *see* what their agent remembered, watch it get smarter across
sessions, and trust it. Demoable, viral, and defensible — and it plays directly
to what's already built.