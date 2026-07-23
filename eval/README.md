# Recall-quality eval harness

Makes AegisDB memory retrieval **measurable** (ROADMAP Horizon 1.1). Seeds a
labelled corpus, runs a query set, and reports `recall@k` and `MRR` with a
per-query breakdown — the scoreboard that every downstream memory-quality change
(extraction, dedup, decay, distillation) should be tuned against.

## Run

```sh
make eval                                   # report only
make eval EVAL_ARGS='--gate-recall-at 5 --gate-threshold 0.8'   # fail on regression
python3 eval/recall_eval.py ./build/aegisdb --json              # machine-readable
```

### Consolidation eval (ROADMAP 2.2)

Measures dedup: seed `--dup-factor` copies of every memory, then check that
`consolidate` collapses the corpus **without losing recall** of the surviving
fact — the whole point of dedup is a smaller corpus at equal (or better) answer
quality.

```sh
make eval EVAL_ARGS='--consolidate'                     # before/after report
python3 eval/recall_eval.py ./build/aegisdb --consolidate --dup-factor 3
```

Exits non-zero if the corpus didn't shrink or recall@maxk regressed. Typical run
on the starter dataset: **66 → 22 records** with recall@10 held at 93% (and
recall@3/@5 improved, since duplicates no longer crowd the top-k).

### Forgetting eval (ROADMAP 2.3)

Measures decay/forgetting: seed the curated facts, flood the corpus with
`--noise` low-value episodic records, then check that `forget` ages out the noise
(corpus plateaus) **without losing recall** of the facts.

```sh
make eval EVAL_ARGS='--decay'                           # before/after report
python3 eval/recall_eval.py ./build/aegisdb --decay --noise 200 --min-retention 0.05
```

Exits non-zero if the corpus didn't shrink or recall@maxk regressed. Typical run:
**222 → 22 records** (200 low-value episodic forgotten) with recall@10 held at
93% — a bounded corpus at equal answer quality.

The default **hashing embedder** is deterministic and dependency-free, so the
harness runs in CI and offline with no model or API. It gives real-but-modest
semantic signal (token overlap → cosine similarity) — enough to make recall
measurable and catch scoring regressions. Absolute numbers are a floor, not a
ceiling: a real embedding model scores higher.

For higher-fidelity runs, plug in a real embedder — it must read text on stdin
and print a JSON array of `embedding_dim` floats:

```sh
python3 eval/recall_eval.py ./build/aegisdb \
    --embedder command --embedder-cmd 'my-embed-cli'
```

## How it works

1. Start a throwaway server at the dataset's `embedding_dim`.
2. Embed and insert every memory; remember `label -> assigned id`.
3. For each query: embed, `search` (with tag/time filters if the query sets
   them), collect the ranked ids, and score against the labelled `relevant` set.
4. Aggregate `recall@k` and `MRR`; print the report (and optionally gate).

The server does not compute embeddings — clients supply them — so the harness
owns embedding for both memories and queries. See `embedders.py`.

## Datasets

`datasets/coding_agent.json` — the coding-agent wedge scenario. Format:

```json
{
  "name": "...", "embedding_dim": 256,
  "memories": [{"label": "...", "type": "semantic", "text": "...",
                "tags": ["..."], "importance": 0.7}],
  "queries":  [{"text": "...", "relevant": ["label1"],
                "tags": ["..."], "match": "all"}]
}
```

`relevant` lists the memory labels that *should* surface for the query. Add
scenarios by dropping in another JSON file and pointing `--dataset` at it.

## A/B task benchmark — does memory *help*? (`ab_tasks.py`)

The recall eval above measures whether the right memory *ranks*. This measures
whether memory changes **task outcomes**: each task teaches a fact in one
"session" (stored in AegisDB), then answers a question in a fresh session two
ways — **ON** (recall + inject the memory) and **OFF** (no memory) — and reports
the success rate of each and the **lift** (ON − OFF). That is the core "is this
useful?" number: if memory doesn't lift success, it isn't earning its tokens.

```sh
make eval-tasks                                   # default: fake model (CI)
make eval-tasks EVAL_ARGS='--model claude-code'   # a real lift, via the claude CLI
make eval-tasks EVAL_ARGS='--model anthropic --judge --min-lift 0.3'
python3 eval/ab_tasks.py ./build/aegisdb --json
```

The **answer model** is a seam (`fake` | `claude-code` | `anthropic` | `openai`,
see `models.py`). The default **`fake`** model answers only from injected
context, so ON succeeds and OFF fails — it proves the harness isolates the memory
effect (ON 100% / OFF 0% / lift +100%) without a model. A **real backend** gives
a real, smaller lift (it can guess some OFF answers and paraphrases ON ones — use
`--judge` for rubric grading rather than keyword match). `--min-lift` gates CI.

Dataset (`datasets/ab_tasks.json`): each task is
`{"id", "memories": [...], "question", "expect_any": [distinctive tokens]}`
(optional `"rubric"` for `--judge`). Each task runs in its own namespace, so the
ON arm only recalls that task's memory.

### Recorded result

`make eval-tasks EVAL_ARGS='--model claude-code --sandbox'`, 10 coding-agent tasks:

```
with memory (ON):    100%
without memory (OFF):  0%
lift:                +100%   (ON − OFF)
```

Memory is the entire difference between a 0% and a 100% agent on cross-session
recall: sandboxed, the model correctly answers "I don't know" on every OFF task
and gets all 10 right when the memory is recalled and injected.

### ⚠ The OFF arm must have no side channel

The lift is only honest if the OFF (no-memory) arm genuinely *cannot* obtain the
answer another way. Two things can leak, and `--sandbox` closes both:

- **Filesystem / tools.** The `claude-code` backend runs `claude -p` **in the
  repo with tool access**, so on a task whose fictional fact matches AegisDB's
  real code, OFF just reads the files and "passes". Without `--sandbox` this run
  scored OFF 20% (`tests` → `make integration`, `style` → `snake_case`, both read
  from source) for a +80% lower bound. `--sandbox` runs from an empty dir with
  tools disabled (`--disallowedTools`), so OFF has nothing to read → OFF 0%.
- **A lenient judge.** `--judge` grades with the model. A naive "is this answer
  correct?" prompt rubber-stamps "I don't know" as acceptable — a sandboxed
  `--judge` run scored OFF 80% purely from that. The grader now grades *factual
  match* and explicitly fails unsure/omitting answers; keyword grading (distinct
  tokens) is the most reliable for a curated dataset.

So: **`--sandbox`** for a clean baseline, and either keyword grading or the
hardened `--judge`. API backends (`--model anthropic|openai`, no filesystem/tools)
are side-channel-free by default; the `fake` model is by construction.