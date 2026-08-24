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
make eval-multihop                          # the symbolic path (5.3)
make eval-extraction                        # prose -> triples (5.4)
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

### Retrieval-mode comparison (ROADMAP 4.1)

Measures the three retrieval paths over one corpus — semantic-only (embedding),
lexical-only (BM25 `query`), and hybrid (both, fused by reciprocal rank) — and
lists the queries hybrid answers that semantic-only misses entirely.

```sh
make eval EVAL_ARGS='--lexical --dataset eval/datasets/identifiers.json'
python3 eval/recall_eval.py ./build/aegisdb --retrieval lexical   # one mode only
```

Exits non-zero if hybrid scores below semantic-only, **or if hybrid loses any
query semantic-only answered** — the interesting failure, since fusion trading
old wins for new ones looks fine in the aggregate.

**Read the numbers with the caveat.** The default `hashing` embedder is itself
token-based, so it behaves much like a lexical matcher and scores far better on
identifier queries than a real dense model would. On
`datasets/identifiers.json` it reports semantic 92% / lexical 100% / hybrid 96%
at recall@1 — so with this embedder the mode is a **regression gate, not a
demonstration of the gap**. For the real gap, point `--embedder command
--embedder-cmd` at an actual embedding model.

This mode earned its keep immediately: it caught the fused ranking being
dominated by the `importance × confidence` multiplier (recall@1 62% vs 92%
semantic-only), because reciprocal-rank scores differ by under 2% between
adjacent ranks and any wider multiplier becomes the primary sort key. Hybrid now
ranks on the fusion alone — see the note in `gather_candidates`.

### Multi-hop eval (ROADMAP 5.3)

The horizon's "done when": questions whose answers live in **no single record**.
Every query asks about a layer; every answer record describes a leaf component,
never names that layer, and avoids the question's vocabulary. The two are
connected only by a chain of `is_a` facts, so there is nothing for similarity or
BM25 to find. The harness seeds the corpus, starts the server with
`--inference`, waits for the derivation to reach a fixpoint, then scores
retrieval and the symbolic path over the same queries.

```sh
make eval-multihop
python3 eval/recall_eval.py ./build/aegisdb --multihop \
    --dataset eval/datasets/multihop.json --gate-recall-at 5
```

Current numbers: **symbolic 100% at every k, retrieval 0% at recall@5** (hybrid
reaches 12% only at recall@10, on a 52-record corpus).

The gate checks **both** directions, and that is the point. A low symbolic score
says the horizon does not deliver. A *high retrieval* score says these questions
were answerable all along, so the comparison proves nothing and the dataset is
not testing what it claims to. `--max-retrieval` (default 0.25) is the second
half.

That second gate is not theoretical: the first version of this dataset failed
it. The answer prose used the same verb as the question — "what does the storage
layer **cap** at?" against "the neighbour-selection loop **caps** its candidate
list at 64" — and lexical search found the answer by that one word, scoring 50%.
The fix was to make the prose say "stops after 64 candidates", which is a fact
and its prose being independent, exactly as ROADMAP 5.2 designed them.

Unlike the retrieval-mode comparison, the hashing embedder's token bias does not
flatter this mode — it makes the retrieval baseline *generous*, since a real
dense model would do no better on a question whose answer shares no words with
it.

### Extraction eval (ROADMAP 5.4) — `extraction_eval.py`

5.4's "done when" is a measurement, not a feature: *triples that snap to the
registry — measured as an in-vocabulary rate, not asserted.* This is that
scoreboard. It runs the real seam (extract → validate → ground → write) over
transcripts paired with the triples a careful reader would write, and reads the
result back **through the server** rather than trusting what the writer returned.

```sh
make eval-extraction                                        # deterministic, gated
python3 eval/extraction_eval.py ./build/aegisdb --extractor claude-code
python3 eval/extraction_eval.py ./build/aegisdb --json
```

Three numbers, and the second and third are deliberately **not summed**:

- **In-vocabulary rate** — accepted over testable. A registry that rejects most
  of what the model proposes and one that fits the corpus look identical from
  the accepted count alone, which is why the ratio is the headline.
- **Conflation** — distinct things sharing one entity id. Facts then describe
  the wrong entity, 5.3 derives more of them, and nothing in the system can
  notice. Unrecoverable, so it **gates at zero**.
- **Fragmentation** — one thing split across ids, counted as **surplus ids**
  (ids beyond one per gold entity) rather than as the number of entities
  affected: an entity splitting three ways and one splitting in two are both
  "1 entity", so the entity count barely moves while the graph gets steadily
  worse. Nothing false is asserted and `consolidate` can merge the records
  afterwards, so it gets a *ceiling* rather than a floor of zero — the design
  prefers this error, and the gate exists to catch a threshold change that
  starts minting for every mention.

  The ceiling is set at the measured baseline (**2 surplus ids**), not loosely.
  Grounding failing outright — `resolve` returning `None` for every mention, so
  each one is re-minted per transcript — produces **5**, and the gate fails.
  That scenario is the reason the gate exists, and an earlier version of it
  reported numbers identical to a healthy run: mentions were recorded at their
  *first* placement only, so the second, different id — which is the entirety
  of what fragmentation is — was dropped before it could be counted.

A single "grounding accuracy" would average an unrecoverable error against a
recoverable one and hide exactly the asymmetry `docs/neuro-symbolic-design.md`
§4 is built around.

**Read the `fake` number with its caveat.** The deterministic backend parses a
line format, not English, so every transcript carries a `cues` block for it.
Under `--extractor fake` the in-vocabulary rate is a property of *the dataset* —
**82.4%** by construction, since three of the seventeen cues name predicates the
registry deliberately lacks. That makes the default run a pipeline regression
gate, not a model score, and the harness prints the caveat on every fake run
rather than trusting anyone to remember it. This is the same discipline as the
retrieval-mode caveat above.

**The number that means something** comes from a real backend. One run of
`--extractor claude-code` over this dataset:

```
IN-VOCABULARY      100.0%     22 proposed, 0 rejected
conflation         0 ids
fragmentation      2 entities → 4 ids
triples vs gold    recall 92.3% (13 expected, 22 held) · 10 beyond gold
```

That is a spot reading of a non-deterministic model, not a gate — but it is the
first evidence for the claim 5.4 rests on: prompted against the registry as a
controlled vocabulary, a model does **not** invent predicates, which is the
standard failure mode of model-built knowledge graphs. The single gold miss is
instructive rather than embarrassing: it wrote `lexical index guarded_by
--no-lexical-index` where gold says `lexical_index.c`, a defensible reading of
the transcript that grounding then kept as a separate entity.

`gold` is a **floor**, not an exhaustive enumeration. Ten triples the store held
were not on the list and are mostly true (`the neighbour-selection loop part_of
hnsw.c`, `append-only log part_of the storage layer`), so they are reported as
*beyond gold* rather than counted as errors — an extractor that reads more of
the transcript must not score worse for it. The gate is on gold **recall** and
never on precision.

Mentions the dataset does not label are reported apart from both grounding
errors. An unlabelled mention says the dataset is incomplete, which is not the
same finding as grounding being wrong, and folding the two together would let a
thin dataset look like a clean run.

`EVAL_ARGS` is appended after the gate flags, so it does **not** relax them
unless it re-passes the same ones. A `--extractor claude-code` run scoring 92%
gold recall sits one missed triple above the 0.9 gate, so pass your own
thresholds alongside it rather than inheriting the deterministic ones.

Not in CI: like every other eval here it needs a built binary, and the real
number needs a model. `make eval-extraction` is the gate to run before touching
extraction, grounding, or the registry.

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

`datasets/identifiers.json` — identifier-heavy recall (flags, env vars,
`file.c:line`, error codes) for the lexical/hybrid comparison above. Same format.

`relevant` lists the memory labels that *should* surface for the query. Add
scenarios by dropping in another JSON file and pointing `--dataset` at it.

`datasets/multihop.json` and `datasets/extraction.json` carry a `predicates`
block that becomes the server's `--predicate-registry` for the run. The
extraction dataset adds `transcripts` — each with `text` (prose, what a real
extractor reads), `cues` (the `S : p : O` lines the `fake` backend reads),
`gold` (the floor of triples a careful reader would write), and `unstatable`
(triples that reader would want and the registry cannot express, kept visible so
"the registry is too small" stays a number rather than an argument) — plus
`entities`, mapping each surface form to the one thing it denotes, which is what
makes conflation and fragmentation separable.

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