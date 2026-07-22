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