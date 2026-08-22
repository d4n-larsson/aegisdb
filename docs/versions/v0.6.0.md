# AegisDB v0.6.0 — keyword search, for the things embeddings miss 🔍

The first feature release since the v0.5.0 beta — everything in the 0.5.x line
was fixes and hardening, hence the minor bump. **`search` can now match the words
a memory actually contains**, ranked by BM25, and fuse that with vector search in
one query. No migration, no on-disk format change; upgrade in place.

## Why

Recall was time, tags, and vector similarity. Nothing matched *text*, so a memory
could not be found by the words in it — and for a coding agent that is a
structural miss. These memories are dense with rare tokens:
`--tenant-max-records`, `hnsw.c:214`, `AEGIS_RECALL_TOP_K`, an error string, a PR
number. Dense embeddings average exactly those away. Ask an embeddings-only setup
for `hnsw.c:214` and you get whatever is topically nearby, not the memory holding
that reference.

Worse, a server with **no embedding provider configured** — which is the Claude
Code recall hook's default — had *no* content-based retrieval at all. Only tags
and time.

## Keyword search

Pass `query` to `search`:

```json
{ "operation": "search", "query": "--tenant-max-records", "top_k": 5 }
```

Or from the CLI, which gained `--query`:

```sh
aegisdb client search --query "hnsw.c:214" --top-k 5
```

The tokenizer deliberately **preserves identifier shape** — `_ - . : / + #` and
non-ASCII bytes stay *inside* a term — so `--tenant-max-records` and `hnsw.c:214`
are indexed whole instead of shredded into unsearchable fragments. A compound
term also contributes its parts, so `records` finds the flag too. Matching is
case-insensitive.

## Hybrid search

Send `query` **and** `embedding` together and the two ranked lists fuse by
reciprocal rank — an exact-term match and a topical match both surface:

```json
{ "operation": "search", "query": "CRC framing", "embedding": [ … ], "top_k": 10 }
```

Ranks are fused rather than scores on purpose: a cosine in [-1,1] and an
unbounded BM25 score share no common scale, and normalising them per query would
make one record's score depend on the rest of the batch.

`explain: true` now reports **which path found each hit**, per hit — so
`"lexical_rank": 1, "semantic_rank": 0` tells you the exact term found this
record and the vector search missed it entirely.

## Claude Code integration

The recall hook and `memory_search` now use keyword search, which changes
behaviour in one way worth knowing about:

- **With embeddings off** (`embedding_mode=none`, the hook's default), recall now
  matches on content instead of falling back to tags/time — and stops reporting
  itself `degraded`, because retrieval genuinely happened. This is the biggest
  practical change in the release.
- **With embeddings on**, recall is hybrid: the server ranks, so the client no
  longer re-sorts by cosine (that would discard the fusion). `recall_min_score`
  is forwarded to the server, where it gates the semantic side *before* fusion.
- `capture`'s supersede detection is deliberately **unchanged** — it filters on a
  cosine floor, and fused scores are on a different scale.

Update the package alongside the server (`uvx --from aegisdb-mcp aegisdb-init`,
or `uvx aegisdb-mcp@0.6.0` if you pin).

## Operator notes

- **Index RAM grows.** A text index over every payload is a new in-memory
  structure. `stats` reports `lexical_terms`, `lexical_docs`, and
  `lexical_bytes`, and it counts toward `--max-index-bytes` like every other
  index. Watch it before/after on a large corpus.
- **`--no-lexical-index`** opts out entirely and reclaims that RAM. A `search`
  carrying a `query` then returns `NOT_READY`; the Claude Code client detects
  this and falls back automatically, so recall keeps working.
- The index is **derived and never checkpointed** — recovery rebuilds it from the
  log like the time and tag indexes. Startup does slightly more work; nothing new
  is written to disk.
- **Backward compatible both ways.** An older client is unaffected. A new client
  talking to an older server gets its `query` field ignored (semantic-only), not
  an error.
- Ranked results now break equal scores on ascending id, so ordering and
  pagination are deterministic. Previously an unstable sort decided ties.

## Measuring it

`make eval EVAL_ARGS='--lexical --dataset eval/datasets/identifiers.json'`
compares semantic-only, lexical-only, and hybrid over one corpus, and gates on
aggregate recall *and* per-query regressions.

It earned its keep during development by catching the fused ranking being
dominated by the `importance × confidence` multiplier — recall@1 of 62%, worse
than either source alone, because reciprocal-rank scores differ by under 2%
between adjacent ranks and any wider multiplier becomes the primary sort key.
Hybrid now ranks on the fusion alone (96% recall@1, MRR 0.979).

**Read those numbers with the caveat**: the eval's default embedder is itself
token-based, so it flatters semantic-only on identifier queries (92% recall@1) —
nothing like a real dense model. With that embedder the comparison is a
regression gate, not a measurement of the true gap. The unambiguous win is
structural: a record with no embedding at all is now retrievable by its terms,
which an embeddings-only server cannot do.

## Known limitation

A hybrid query does not apply `importance × confidence` weighting or
`half_life_ms` recency decay (it reports both as `1.0` in `explain`), for the
reason above. Use a single-source query when you want that shaping. Fusing
*weighted* ranks needs the records loaded before fusion rather than after — a
real restructure of the candidate path, tracked in
[ROADMAP.md](ROADMAP.md) (Horizon 4.1).

## Links

- Wire protocol: [wire-protocol.md](wire-protocol.md) — `query`, fusion, `explain`
- Architecture: [architecture.md](architecture.md#lexical-retrieval-and-fusion)
- Roadmap: [ROADMAP.md](ROADMAP.md) — Horizon 4

*No migration required. MIT licensed. Built in C17, no runtime dependencies.*