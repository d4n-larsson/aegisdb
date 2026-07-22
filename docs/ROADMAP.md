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

## Horizon 1 — Now: trust the wedge

*Theme: make the coding-agent memory use case demoable and trustworthy end to
end. Nothing here needs a model on the hot path.*

### 1.1 Recall-quality eval harness *(foundational — do first)*
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

### 1.2 Provenance on every retrieved memory
- **Why now:** trust starts with "where did this come from and when?" Cheap given
  the data already stored.
- **Build:** surface per-hit provenance in `search`/`recall` responses — source,
  `created`/`updated`, supersession chain, and a **score breakdown** (why this
  ranked here: semantic vs. recency vs. importance contribution).
- **Leverages:** existing record metadata, relationships, the semantic
  supersession already in the log.
- **Done when:** every hit can answer "why am I seeing this?" without server-side
  guesswork.

### 1.3 Memory-inspection surface
- **Why now:** the single most effective *sales/adoption* artifact — let a human
  see and correct what the agent believes. The repo already has dashboard muscle
  (Grafana).
- **Build:** a browse/search/inspect view over a namespace: list memories, see the
  1.2 score breakdown and provenance per hit, edit/delete. Start as a
  self-contained artifact/dashboard reading the existing `stats`/`search` surface.
- **Done when:** a Claude Code user can watch memory accumulate across sessions
  and hand-correct a wrong memory.

---

## Horizon 2 — Next: memory that stays coherent

*Theme: the memory-quality layer. Turn existing primitives into real policy.*

### 2.1 Extraction (write-path quality)
- **Why now:** today the caller decides what to insert. Owning "turn this
  conversation into durable facts worth keeping" is the difference between a DB
  and a memory *product* (mem0's core pitch).
- **Build:** an extraction step in the integration (provider-neutral, off the hot
  path) that distils turns into candidate facts with importance/confidence, then
  writes them through the dedup/contradiction policy below.
- **Leverages:** the existing summarizer provider seam
  (`none`/`fake`/`claude-code`/`anthropic`/`openai`) — reuse it, don't reinvent.

### 2.2 Dedup + contradiction resolution
- **Why now:** unbounded, self-contradicting memory is worse than none.
- **Build:** a policy layer over the mechanical `consolidate`:
  near-duplicate collapse (already have cosine dedup), plus **supersession** when
  a new fact contradicts an old one ("prefers X, *not* Y"), recording the
  supersession chain for provenance (1.2).
- **Leverages:** `consolidate`, `MEM_SEMANTIC` supersession in the append-only
  log, `update`/`delete`.

### 2.3 Decay & forgetting policy
- **Why now:** recall is injected every turn — its size is a recurring token cost,
  and indexes are in RAM. Forgetting is a feature.
- **Build:** importance × recency decay with configurable half-life; promote
  durable facts, let low-value episodic events age out. Complements the
  already-implemented **LLM distillation** (`summarization-design.md`) which folds
  related aging clusters into one dense fact.
- **Leverages:** TTL sweep, `promote`, the background summarizer.
- **Done when:** a long-running agent's recall size and index RAM plateau instead
  of growing linearly, with recall@k held (measured by 1.1).

---

## Horizon 3 — Later: enterprise trust & scale

*Theme: the capabilities that turn "interesting" into "we can deploy this."*

### 3.1 Temporal / bitemporal queries
- **Why now:** "what did the agent know at time T?" is a real differentiator (part
  of Zep's pitch) and enterprises pay for it. The append-only log already contains
  the history.
- **Build:** point-in-time reads over the log's history; validity intervals on
  facts (valid-from/valid-to) so superseded facts remain auditable rather than
  overwritten.
- **Leverages:** the append-only log, snapshots, recovery replay.

### 3.2 Right-to-be-forgotten & export
- **Why now:** table stakes for any B2B memory sale. For a *memory* product,
  "forget everything about user X" and "export what you store about me" are not
  optional.
- **Build:** compliance-grade hard delete scoped by namespace (verified through
  compaction so data actually leaves disk), plus a per-subject export.
- **Leverages:** namespaces/tenancy, compaction, snapshots.
- **Done when:** a hard-delete provably removes a subject's data from the log and
  all indexes after compaction, with a test that greps the on-disk log.

### 3.3 Hosted tier & operability
- **Why now:** distribution. The C core's durability/encryption/replication become
  a sales line only if it's trivial to run.
- **Build:** one-line deploy (the `docker-compose.yml` is step one), first-class
  metrics (extend the existing Grafana dashboard: recall latency, index RAM,
  eviction/decay rates, distillation lag), SDKs, and framework adapters beyond MCP.
- **Leverages:** replication, snapshots/restore, health/stats endpoints, encryption.

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
```

Build the **scoreboard (1.1)** before the memory-policy work in Horizon 2, so
extraction, dedup, and decay are tuned against numbers rather than vibes. Ship
**provenance + inspection (1.2/1.3)** early because trust — not features — is what
gates adoption of the wedge.

## Risks & how the roadmap answers them

- **Commoditization from above** (model providers ship native memory, frameworks
  bundle it). *Answer:* be better and *inspectable* at the policy + trust layer
  (Horizons 1–2), and stay provider-neutral.
- **Scope drift into a generic vector DB.** *Answer:* the non-goals, and keeping
  every workstream tied to the coding-agent wedge until it's won.
- **Polishing the engine because it's concrete** while the unglamorous
  extraction/eval/UI work — where the users are — waits. *Answer:* Horizon 1 is
  deliberately the eval + trust surface, not more storage features.

## The one-line bet

Go deep on **coding-agent memory + a memory-inspection/eval surface**: let a
Claude Code user *see* what their agent remembered, watch it get smarter across
sessions, and trust it. Demoable, viral, and defensible — and it plays directly
to what's already built.