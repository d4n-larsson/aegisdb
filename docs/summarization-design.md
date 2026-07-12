# Design: Background Summarization (memory distillation)

**Status:** PR 1 implemented (this PR). The provider seam (`none`/`fake`/
`claude-code`), the core distiller, `--dry-run`, and the operator-scheduled
`aegisdb-summarize` entry point are in the Claude Code integration; the C server
is unchanged. Decisions taken: the trigger is **operator-scheduled** (not a
per-turn hook), and the first real backend is **`claude-code`** (shells to the
`claude` CLI — the agent's own model, no API key). Remaining: a direct
API backend (`anthropic`/`openai`) and scheduling/docs polish.
**Scope:** Periodically fold clusters of related, aging memories into a single
distilled fact, so recall stays small and cheap as memory accumulates — and the
in-RAM indexes stop growing without bound. This is the LLM-powered complement to
the server's existing mechanical `consolidate` (which only merges near-identical
vectors).

## 1. The problem

Recall is injected into every turn, so its size is a recurring token cost, and
the indexes are held in RAM, so total memory grows with the corpus. Two forces
push the corpus up over time:

- **Volume.** A busy agent accumulates thousands of episodic events — many of
  them individually low-value but collectively meaningful ("ran the deploy",
  "deploy failed", "fixed the deploy" → *"deploys go through `make ship`; the
  staging step is flaky"*).
- **Redundancy that isn't exact.** `consolidate` already merges *near-duplicate*
  vectors (cosine ≥ threshold). It does **not** touch memories that are *related
  but distinct* — the case where a short summary would replace ten records.

Mechanical dedup can't write that summary; it takes a model. Nothing in AegisDB
has one today, which is the crux of this design.

## 2. Goals & non-goals

**Goals**
- Reduce the standing recall/token cost and index size by distilling clusters of
  related memories into fewer, denser semantic facts.
- Keep it **opt-in** and **off the hot path** — never in a prompt's critical
  latency, never on by default, never a new hard dependency for the base server.
- Preserve **provenance** and be **recoverable**: a distilled fact links to its
  sources, and the sources are archived (not destroyed), so a bad summary is
  reversible and auditable.
- Stay in AegisDB's grain: the C server stays dependency-free and LLM-free; the
  "smart" layer is the integration, exactly as recall/capture already are.

**Non-goals**
- **No LLM in the C server.** It must stay a small, no-deps binary. Summarization
  lives in the integration (or an operator job), not in `aegisdb`.
- **Not real-time.** This is a periodic maintenance pass, not a per-turn step.
- **Not automatic deletion.** Distillation archives sources (TTL/tombstone), it
  does not hard-delete the episodic source of truth.
- **Not a general agent framework.** One narrow task: cluster → summarize →
  write back with provenance.

## 3. The core decision — where the model lives

AegisDB's server has no model and must not gain one. The realistic options:

| Option | Where | Model | Verdict |
|---|---|---|---|
| A. Server-side | `aegisdb` C process | vendored/linked LLM | ✗ breaks the no-deps, single-binary ethos; absurd |
| B. Integration job | Python (`aegis_mcp`), scheduled | a configured chat model (API or local) | ✓ **recommended** — mirrors how recall/capture already work |
| C. Agent-driven | a Claude Code slash-command / subagent | the agent's *own* model | ✓ viable v2 — no extra dep, but interactive, not "background" |

**Recommended: B, with C as a later convenience.** Add a `SummaryProvider`
abstraction alongside the existing `EmbeddingProvider`:

- `none` (default) — feature disabled; no dependency, no cost, no behavior change.
- `anthropic` / `openai-compatible` — opt-in chat backend behind an API key
  (lazy-imported like the Voyage provider, so importing the module never
  requires the SDK).
- (v2) `claude-code` — emit a slash-command/skill so the agent distills its own
  memories with no external key; documented as an alternative trigger.

It runs as an **explicit, scheduled job** — a new `aegisdb-mcp summarize`
entry point (sibling to `aegis-recall-hook` / `aegis-capture-hook`), driven by
cron / a systemd timer / the compose sidecar — **not** an autonomous daemon and
**not** wired into a per-session hook by default. Autonomous, unattended LLM
calls over untrusted memory content are a cost and a safety liability (§6); an
operator-scheduled, budgeted, dry-runnable job is the safe shape for v1.

## 4. How it works

1. **Select candidates (server-side, cheap, no model).** Per namespace, find
   clusters worth distilling:
   - *Aging + low-value*: episodic records older than a window and below an
     importance floor.
   - *Topically related*: group by shared tags and/or semantic neighborhood
     (reuse `search` to gather a cluster around a seed), excluding anything
     already summarized (§7).
   - Bound the pass: at most N clusters and M records/cluster per run.
2. **Summarize (model).** For each cluster, prompt the `SummaryProvider` with the
   member texts and ask for one terse, factual semantic memory — the same
   "caveman"/terse style capture already favors — plus a confidence. Inputs are
   **untrusted** (§6): the prompt frames them as data to summarize, never as
   instructions.
3. **Write back (server-side).**
   - `insert` the summary as a **semantic** memory (tags = union of the cluster's
     tags + a `summary` marker; importance = max of the cluster; `agent_id` = the
     namespace).
   - `relate` the summary to each source id (`kind: "summarizes"`) so provenance
     is a graph edge — traversable, auditable.
   - **Archive** the sources. *(v1: tombstone them via `delete`.* In this
     log-structured store a delete is a soft tombstone — dropped from recall,
     reclaimed by compaction, but still in the log tail for audit/rollback until
     then, so it is not a hard delete. A TTL-on-existing-record path would be
     tidier but `update` has no TTL field today; that's a possible small server
     addition later.)
4. **Report.** Log clusters found / summarized / records archived; support
   `--dry-run` that does steps 1–2 and prints the proposed summaries without
   writing anything.

## 5. Interaction with what exists
- **`consolidate`** stays as the cheap, safe, always-available mechanical dedup.
  Summarization is the heavier, opt-in, LLM tier layered on top — run
  `consolidate` first (collapse exact dups), then summarize the distinct remainder.
- **Graph** carries provenance (`summarizes` edges); `traverse` from a summary
  reaches its sources.
- **TTL / compaction** do the reclamation; summarization only sets the horizon.
- **Recall dedup** (`AEGIS_RECALL_DEDUP_THRESHOLD`) already suppresses redundant
  recall at read time; summarization reduces the corpus at rest. Complementary.

## 6. Safety

- **Hallucination.** Summaries are lossy and a model can invent. Mitigations:
  provenance edges to every source; sources archived (recoverable), not deleted;
  a confidence floor below which the cluster is left untouched; `--dry-run` for
  operator review; conservative cluster sizes.
- **Prompt injection / memory poisoning.** This is the deferred security item
  from the review (transcript-derived memories are untrusted): summarizing
  attacker-influenced content feeds it to a model. The prompt must treat cluster
  contents strictly as data ("summarize the following memories" with clear
  delimiting), never execute instructions found in them, and the summary inherits
  the "untrusted" labeling. A summary is not more trusted than its sources.
- **Idempotency / runaway cost.** Mark summarized sources (a tag / the archive
  state) and exclude already-summarized clusters, so repeated runs converge
  instead of re-summarizing (and re-billing) the same content. Bound each run by
  a cluster cap and, ideally, a token budget.
- **Privacy.** An opt-in external chat backend sends memory content to a third
  party — document it prominently; `none` (no external calls) stays the default.

## 7. Marking & idempotency
A distilled summary is tagged `summary`; its sources are tagged (e.g.
`summarized`) and TTL'd. Candidate selection excludes records tagged
`summarized` and clusters that already have a `summarizes`-linked summary, so the
pass is convergent and re-runnable.

## 8. Configuration (integration)
- `AEGIS_SUMMARY_MODE` — `none` (default) | `anthropic` | `openai` | (v2) `claude-code`.
- `AEGIS_SUMMARY_MODEL`, and the backend's API key via its standard env var.
- `AEGIS_SUMMARY_MIN_AGE_MS`, `AEGIS_SUMMARY_MAX_IMPORTANCE`, `AEGIS_SUMMARY_MIN_CLUSTER`,
  `AEGIS_SUMMARY_MAX_CLUSTERS_PER_RUN`, `AEGIS_SUMMARY_ARCHIVE_TTL_MS`,
  `AEGIS_SUMMARY_MIN_CONFIDENCE`.
The C server is unchanged; everything here is integration config.

## 9. Rollout (PR sequence)
1. ✅ **(this PR)** `SummaryProvider` abstraction + `none`/`fake`/`claude-code`
   backends + the `summarize` core (candidate selection, cluster→summary→
   write-back with provenance + tombstone archive) + `--dry-run` + the
   operator-scheduled `aegisdb-summarize` entry point. Testable end-to-end with
   the deterministic fake summarizer against a real server; no external
   dependency. The `claude-code` backend shells to the `claude` CLI, so the first
   *real* summarizer needs no API key.
2. **A direct chat backend** (`anthropic` / openai-compatible), lazy-imported,
   behind a key — for environments without the Claude Code CLI.
3. **Scheduling polish** — a compose `summarize` sidecar / cron + systemd-timer
   examples, and docs.

## 10. Open questions
- **Server help for candidate selection?** Selection is doable today with
  `search` + `count` + tag filters, but an "oldest N below importance X in
  namespace" query would be cheaper than client-side gathering. Worth a small
  read-only server op? (Keeps the LLM out of the server while making selection
  efficient.)
- **Trigger default.** Purely operator-scheduled (safest), or an opt-in
  "summarize at session end if the namespace exceeds N memories" capture-hook
  extension (more automatic, more cost surface)?
- **Cross-tenant.** Runs per namespace; a shared server would summarize each
  tenant independently under that tenant's config/key.