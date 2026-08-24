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

## Horizon 5 — Next: reasoning over memory, not just retrieving it

*Theme: the relationship graph is the one primitive in the tree that was built
and then never finished. Horizons 1–2 made memory coherent, 4 made it findable;
this makes it **inferrable** — and it is the only way to answer the question the
North Star actually asks at the level of **belief** rather than **ranking**.*

**The division of labour.** Symbols where the answer must be reproducible,
auditable, and cheap; the model where the input is ambiguous. The LLM is the
*interface* to the symbolic store — parsing prose into facts, verbalizing a
derivation back into English, adjudicating what the rules flagged but could not
settle. It is never *inside* the inference loop, because an inference the model
performs is one nobody can regression-test. Note that today the arrangement is
**inverted**: contradiction detection is an LLM call per candidate fact
(`judge_supersedes`, 2.1) doing work that a single-valued-predicate constraint
does deterministically, at write time, for free.

### 5.1 A queryable relationship graph *(foundational — do first)* — *shipped (`traverse` `kinds`/`direction`/`traversal`; see `symbolic-layer-design.md`)*

- **Why now:** `kind` is inert. `qe_traverse` enqueues every outgoing neighbour
  with no kind filter (`query_engine.c:741`) and there is no reverse adjacency,
  so the two edge kinds already written in anger — `supersedes` from
  consolidation (`qe_maint.c:441`) and `derived_from` — can be walked neither
  selectively nor backwards. "Show me the supersession chain", which 1.2 counts
  as shipped provenance, is really a blind BFS plus client-side filtering;
  "what depends on this?" is a full scan. Every later item in this horizon needs
  a graph you can ask questions of.
- **Build:** an edge index (kind → postings, plus reverse adjacency) alongside
  `tag_index`; `kinds: [...]` and `direction: out|in|both` on `traverse`; the
  traversed `kind` reported per hop so a path is legible rather than inferred.
  Derived and in-RAM, rebuilt from the log like time/tag/lexical — no new
  checkpoint, nothing new to corrupt.
- **Leverages:** `tag_index`'s inverted-postings machinery (`tag_index.h` — the
  exact pattern `lexical_index` followed in 4.1), `relate`'s existing edge
  idempotency, and `stats`'s per-index byte reporting so its RAM is watchable
  like every other index.
- **Falls out for free:** the inspector (1.3) can render a provenance *tree*
  instead of a flat neighbour list, and `export` (3.2) can follow a subject's
  derivation lineage.
- **Done when:** the supersession chain of a consolidated record is retrievable
  in one `traverse` call, backwards, without a scan — and a contract test
  asserts an unrequested edge kind is not followed. ✅
- **Shipped:** `traverse` takes `kinds` (union, capped at 16) and `direction`
  (`out`/`in`/`both`), and every returned record carries a `traversal` object
  (`depth`, `via_id`, `via_kind`, `via_direction`) so a walk reads as a path
  rather than a set. Filtering and the forward walk need **no new state** — the
  record is its own forward adjacency list — so only the reverse direction is
  backed by an index (`edge_index.h/.c`: `to_id` → incoming sources, interned
  kinds, derived and never checkpointed, rebuilt by recovery, `--no-edge-index`
  opts out and makes a reverse walk `NOT_READY`). `stats` reports
  `edges`/`edge_kinds`/`edge_bytes`, counted toward `--max-index-bytes`.
- **Bounded:** a traversal visits at most `TRAVERSE_MAX_NODES` (8192) records
  and reports `capped` when it stops early. Reverse traversal is what made this
  necessary: outdegree is capped per record at 4096, indegree at nothing, so a
  backward walk from a heavily-referenced record was unbounded work under the
  index read lock.
- **Cost, measured:** dominated by the *target* table rather than the postings,
  so it tracks fan-in — ~121 B/edge at one source per target (the provenance
  shape), ~17 at a thousand. The design doc carries the table and two further
  levers; this was a guess in the design and a measurement in the end.
- **Cost to state plainly:** edges live *inside* the record (`record.h:11`), so
  every `relate` rewrites the whole record to the log, capped at
  `MAX_RELATIONSHIPS` (4096, `qe_internal.h:12`). That is right for
  provenance-density and wrong for knowledge-graph density. 5.1 does not change
  it; 5.2 cannot avoid confronting it.

### 5.2 Typed facts & a predicate registry — *shipped (`fact` on `insert`, `pattern` on `search`; see `typed-facts-design.md`)*

- **Why now:** `data` is an opaque blob, so there is nothing to unify against
  and no way to state a constraint. Tags are a flat set with no subsumption, so
  nothing inherits. Without a symbol there is no symbolic layer — only a graph.
- **Build:** **the triple *is* the record** — no second store. `insert` accepts
  an optional `fact: {s, p, o}` (object an id-ref or a literal) beside the
  natural-language rendering that stays in `data`, indexed by `(s,p)` and
  `(p,o)`. Plus a **predicate registry**: a declarative schema, loaded from
  config, carrying per-predicate cardinality (single- vs multi-valued),
  symmetry, transitivity, inverse-of, and mutex sets. `search` gains a
  `pattern` filter with wildcards.
- **Leverages:** everything already built applies unchanged — the append-only
  log becomes the belief history, `history`/`as_of` (3.1) becomes bitemporal
  *belief*, namespaces are per-agent belief sets, `forget` (2.3) decays derived
  facts, and 4.1's identifier-preserving tokenizer already makes predicate and
  entity names findable by their exact spelling.
- **Non-goal check:** a `pattern` filter is one more field on `search`, the same
  shape as 4.1's `query` — *not* a query language. The rules stay a
  **registry**: declarative config, never syntax. If this item starts growing a
  grammar, it has drifted into the non-goals and should be cut back.
- **Done when:** a fact written as a triple is retrievable by pattern
  (`{"s": 42, "p": "prefers", "o": "*"}`), the registry loads from config, and a
  record with no `fact` behaves exactly as it does today.
- **Designed:** `typed-facts-design.md`. Two things the design settled that the
  entry above left open. The subject is a **record id**, not a bare symbol, so
  tenant isolation over facts is the isolation already shipped rather than a
  second mechanism — at the cost of needing an "entity record" convention for
  things that are not memories. And this is the first Horizon 5 item to touch the
  **durable** record format: codec v3 adds the triple, but a record with no fact
  still encodes as v2 byte-for-byte, so a deployment that never writes a fact is
  unchanged on disk and on the wire. That also confines a format change nobody
  can downgrade past to the deployments that opted in.
- **Found while designing:** the replication handshake negotiated no codec
  version at all, so a primary writing a newer record format streamed frames an
  older replica could only reject, frame by frame, with no way to say why. Fixed
  as part of this horizon: the handshake now carries a `codec_version` and the
  primary withholds a frame the replica cannot decode, naming the offset and
  both versions. Worth having regardless of facts.
- **Shipped:** codec v3 carries an optional `{s, p, o}` triple, and a record
  with no fact still encodes as v2 byte-for-byte — so a deployment that never
  writes a fact is unchanged on disk and on the wire, and a format change nobody
  can downgrade past is confined to those who opted in. Three derived indexes
  (`fact_index.h/.c`: subject, object, predicate) answer any non-empty subset of
  the triple; `--predicate-registry` declares the vocabulary and a `fact` naming
  an undeclared predicate — or the wrong object kind for it — is refused at
  insert. `search` and `count` take a `pattern`; bulk `delete` refuses one rather
  than quietly ignoring it. `stats` reports
  `facts`/`fact_predicates`/`registered_predicates`/`fact_bytes`.
- **Held to the non-goal:** `pattern` has no variables, no disjunction and no
  joins, so it stays a filter on an existing op rather than the beginning of a
  query language. `cardinality`, `symmetric`, `transitive`, `inverse_of` and
  `mutex_with` are declared and validated for coherence but nothing acts on
  them — that is 5.3.

### 5.3 Deterministic inference & truth maintenance — *shipped (`--inference`; see `inference-design.md`)*

- **Why now:** this is where the trust payoff lands. `explain` (1.2) explains
  *ranking* — similarity, BM25, RRF, recency. It cannot say "you believe this
  because of these three facts and this rule." A derivation tree can. Derivation
  is also what makes forgetting *safe*: today, tombstoning a premise leaves its
  consequences standing and nothing notices.
- **Build:** three closures, and deliberately no more —
  1. **subsumption** over an `is_a` taxonomy, so a memory about `hnsw.c:214`
     answers a question about the storage layer with no embedding involved;
  2. **transitive / symmetric / inverse** closure over predicates the 5.2
     registry declares as such;
  3. **functional-property conflict detection** — two live values for a
     single-valued predicate is a contradiction, found deterministically.

  Materialize forward in a **background job**, never the write path (the
  summarizer/compaction precedent), depth- and fanout-capped. Every derived
  record carries `derived_from` edges to its premises plus the rule that fired,
  so an inference is provenance-linked by construction and `explain` can walk
  it. Confidence propagates as the product along the chain with a floor —
  testable, and honest about being a heuristic rather than a probability. Truth
  maintenance then falls out of 5.1's reverse adjacency: retract a premise, walk
  `derived_from` backwards, tombstone the dependents.
- **Leverages:** 5.1's edge index, the `supersedes`/`derived_from` vocabulary
  already in use, `forget`/`consolidate` as the retraction mechanism, and the
  append-only log as the truth-maintenance journal.
- **Done when:** `make eval` gains a multi-hop dataset whose queries are
  **structurally unanswerable by retrieval alone** (the answer lives in no
  single record), and the symbolic path answers them while semantic, lexical,
  and hybrid all score near zero. Plus: retracting a premise demonstrably
  retracts its consequences, and a functional-property contradiction is caught
  with no model call.
- **Not building:** a general Datalog/Prolog engine in C. Unrestricted
  recursion, negation-as-failure, and a rule language are all out of scope; the
  three closures above cover the coding-agent cases and stop there.
- **Deferred:** non-monotonic defaults and exceptions ("normally X, except
  here"). Supersession already covers the common case, and defeasible reasoning
  is a research rabbit-hole with a poor ratio of value to subtlety.
- **Designed:** `inference-design.md`. Three things the design settled that the
  entry above left open. **Subsumption is not a closure** — materializing it
  would write facts that are false (the storage layer does not default to what
  `hnsw.c` defaults to) and would go quadratic in taxonomy depth × facts per
  entity; it becomes an opt-in `subsume` flag on `pattern` instead, reading the
  `is_a` closure that *is* materialized, so the two compose into one index
  probe. A conclusion is a **record**, carrying codec v4's `derivation` (rule,
  depth, premise ids) — server-only and unforgeable, because every trust claim
  in this horizon rests on provenance a client cannot manufacture. And the job
  runs **per namespace**: the fact indexes are server-wide, so the naive
  implementation would join a premise from one tenant with a premise from
  another and write a record that exists in neither.
- **Shipped:** codec v5 carries a `derivation` — a *set* of independent
  justifications, because support is disjunctive and a flat premise list cannot
  answer the question retraction has to ask. `--inference` (off by default,
  since it grows the log rather than RAM) materializes the transitive, symmetric
  and `inverse_of` closures on the maintenance tick, per namespace, never on a
  replica. Retraction withdraws a conclusion when *every* route has lost a
  premise, follows `supersedes` so a merged premise is not mistaken for a lost
  one, and is reconstructed by recovery rather than persisted. `cardinality` and
  `mutex_with` produce `conflicts_with` edges and a `conflicts` gauge —
  reported, never resolved. `subsume` on `search`/`count` broadens a subject
  through `is_a`, and `explain.derivation` says why a record is believed.
- **Done, measured:** `make eval-multihop` reports **symbolic 100% against
  retrieval 0% at recall@5** on questions whose answers live in no single
  record. The gate bounds retrieval as well as the symbolic path, which caught
  the first dataset answering 50% of its own questions by word overlap.
- **Found while designing:** retraction does *not* fall out of 5.1's reverse
  adjacency as the entry assumes. `qe_delete` drops every edge pointing at the
  tombstone before it returns, so a background job walking `derived_from`
  backwards on the next tick finds nothing. Fixed by capturing dependents under
  the write lock `qe_delete` already holds and draining them off it — with the
  queue deliberately *not* durable, because a derived record names its own
  premises and recovery can reconcile the whole live set instead. That turns
  "the queue was flushed" into the far better invariant "no live derived record
  has a dead premise", which is checkable on every restart.

### 5.4 The neuro-symbolic seam

- **Why now:** symbols are only worth having if writing and reading them is as
  easy as writing prose. That is exactly what a model is for — and the seam to
  hang it on already exists and is already provider-neutral.
- **Build:** three jobs for the model, all **at the boundary**, none inside
  5.3's loop —
  1. **Parse (write path)** — prose → candidate triples. Extend the
     `ExtractionProvider` seam (`aegis_mcp/extract.py`, 2.1) with a triple
     target, prompted **against the 5.2 registry as a controlled vocabulary**.
     This is the item that decides whether the horizon works at all: a model
     inventing predicates freely produces a symbol soup no rule can ever fire
     on, which is the standard failure mode of model-built knowledge graphs.
  2. **Verbalize & formulate (read path)** — question → `pattern` filter, and
     derivation tree → an English "here is why I believe this." The model
     *reads* the proof; it never produces it.
  3. **Adjudicate (fallback)** — when 5.3 detects a conflict it cannot settle
     (two confident values for a single-valued predicate), hand *that one case*
     to the model. Symbolic detection, neural resolution — the inverse of
     today's arrangement, and cheaper: the model sees only the hard cases
     instead of every candidate fact.
- **Grounding is the hard part.** Two phrasings of one fact must become one
  symbol or the store fragments. The fix reuses shipped machinery rather than
  inventing any: resolve a mention to an existing entity id via HNSW cosine +
  BM25 before minting a new symbol — the same candidate-and-collapse shape as
  2.2's dedup, scored by the same harness.
- **Leverages:** the extraction/summarization provider seam
  (`none`/`fake`/`claude-code`/`anthropic`/`openai` — the deterministic `fake`
  backend is what makes any of this testable), 4.1's hybrid retrieval for entity
  resolution, and `consolidate` for collapsing duplicate symbols.
- **Done when:** a coding-agent transcript produces triples that snap to the
  registry — measured as an in-vocabulary rate, not asserted — and a wrong
  inference is traceable to either a bad premise or a bad parse, never to an
  opaque model judgment.
- **Designed:** `neuro-symbolic-design.md`. Three things the design settled that
  the entry above left open. **Grounding is deliberately biased toward
  fragmentation**: conflating two entities writes facts about the wrong thing
  and 5.3 then derives more of them, with nothing able to detect it, while
  splitting one entity in two only loses inferences and `consolidate` can merge
  them afterwards — now that a merge preserves assertions. One error is
  recoverable and the other is not, so a near-miss mints rather than guesses.
  **A rejected triple is dropped and counted, never coerced** onto the nearest
  declared predicate: coercion would convert the in-vocabulary rate, which is
  the number this item is judged on, into silent corruption of what the corpus
  asserts. And **nothing is lost by rejection** — the prose stays in `data` and
  stays searchable, so an extraction that yields no triple degrades to exactly
  what 2.1 does today.
- **Sharpened:** the model reads the proof and never produces it. Verbalization
  renders an `explain.derivation` that already exists and can be checked against
  the record, rather than an explanation generated alongside an answer — which
  is the arrangement that makes model reasoning unfalsifiable.
- **Measured (`make eval-extraction`):** the "done when" above is a number, so
  it is now one. `eval/extraction_eval.py` runs the real seam over transcripts
  paired with the triples a careful reader would write, and reads the result
  back *through the server* rather than trusting the writer's own return value.
  One run of `--extractor claude-code`: **in-vocabulary 100%** (22 proposed, 0
  rejected), **conflation 0**, **fragmentation 2 entities → 4 ids**, gold recall
  92%. That is a spot reading of a non-deterministic model rather than a gate,
  but it is the first evidence for the claim this item rests on — prompted
  against the registry as a controlled vocabulary, a model does not invent
  predicates, which is the standard failure mode of model-built knowledge
  graphs. The gate itself runs the deterministic `fake` backend, whose
  in-vocabulary rate is a property of the dataset (82.4% by construction, three
  of seventeen cues naming predicates the registry deliberately lacks) — a
  pipeline regression gate, and the harness says so on every fake run rather
  than printing a number that looks like a model score.
- **Held to the asymmetry:** the two grounding errors are gated *apart* and
  never summed. Conflation gates at zero because nothing downstream can detect
  it; fragmentation gets a ceiling rather than a floor, because the design
  prefers that error and the gate is there to catch a threshold change that
  starts minting for every mention. A single "grounding accuracy" would average
  the unrecoverable error against the recoverable one and hide the argument.
  Likewise `gold` is a floor, not an enumeration: ten triples the store held
  were not on the list and are mostly true, so they are reported as *beyond
  gold*, and the gate is on recall and never on precision — an extractor that
  reads more of the transcript must not score worse for it.
- **Remaining:** adjudication (§6 of the design) — hand a contradiction 5.3
  flagged but refused to settle to the model, and write the verdict as a
  `supersedes`. Blocked on a smaller decision the design left open: `conflicts`
  is a gauge recomputed each tick and `conflicts_with` edges are only reachable
  by `traverse` from an id you already hold, so **there is no way to enumerate
  the flagged pairs**. The pass already computes them; keeping that set is the
  cheap answer.

### Ground rules for the whole horizon

- **Off by default; degrade to today.** Every piece is opt-in, and recall is
  never gated on the symbolic path. A brittle reasoner that fails closed is
  worse than no reasoner — this is the `--no-lexical-index` discipline in
  reverse.
- **Unmeasured is unshipped.** By this repo's own standard (1.1 before Horizon
  2), 5.3's multi-hop eval dataset lands *before or with* the inference, not
  after it.
- **RAM is a budget.** Closure materialization can multiply the corpus. Cap it,
  report it in `stats` like every other index, and surface it in the dashboard
  (3.3).

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
        │
5.1 edge index ──> 5.2 typed facts ──> 5.3 inference + TMS
        │                                      │
        └──────────> 5.4 neuro-symbolic seam <─┘
        (graded by a new multi-hop dataset in the same harness)
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

**Horizon 5 starts with 5.1 alone**, and 5.1 is worth shipping even if nothing
after it ever is: it is pure engine work with no model on any path, and it
retroactively fixes provenance walks for features already shipped (1.2's
supersession chain, 1.3's inspector). Only then a *narrow* 5.2/5.3 spike over
one predicate family — enough to see whether the multi-hop eval moves before
committing to the layer. The model work (5.4) comes last because it is the
easiest part to demo and the easiest to fool yourself with: without the registry
to snap to, extracted triples look impressive and reason over nothing.

## Risks & how the roadmap answers them

- **Commoditization from above** (model providers ship native memory, frameworks
  bundle it). *Answer:* be better and *inspectable* at the policy + trust layer
  (Horizons 1–2), and stay provider-neutral.
- **Scope drift into a generic vector DB.** *Answer:* the non-goals, and keeping
  every workstream tied to the coding-agent wedge until it's won. Note that 4.1
  is *not* an exception: hybrid retrieval is one `query` field on the existing
  `search` op — not a query language, and not full-text search as a product —
  scoped to a recall failure the wedge hits daily.
- **Horizon 5 turns into a knowledge-graph product** — a rule language, a
  reasoner, a graph query dialect. This is the largest scope-drift risk in the
  document, because the subject matter invites it. *Answer:* the explicit "not
  building" list in 5.3, rules as declarative *registry* rather than syntax in
  5.2, `pattern` as one `search` field in the shape of 4.1's `query`, and the
  standing test — if it grows a grammar, cut it back.
- **The symbol space rots into soup.** A model minting predicates and entities
  freely produces a graph no rule can fire on, which is how most LLM-built
  knowledge graphs die. *Answer:* 5.2's registry as a controlled vocabulary,
  entity resolution reusing 2.2's shipped dedup machinery, and an in-vocabulary
  rate measured like every other number here rather than assumed.
- **A brittle reasoner degrades recall** for the users it was meant to help.
  *Answer:* Horizon 5's ground rules — opt-in, off by default, recall never
  gated on the symbolic path.
- **Polishing the engine because it's concrete** while the unglamorous
  extraction/eval/UI work — where the users are — waits. *Answer:* Horizon 1 is
  deliberately the eval + trust surface, not more storage features.

## The one-line bet

Go deep on **coding-agent memory + a memory-inspection/eval surface**: let a
Claude Code user *see* what their agent remembered, watch it get smarter across
sessions, and trust it. Demoable, viral, and defensible — and it plays directly
to what's already built.