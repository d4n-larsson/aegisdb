# Design: A Queryable Relationship Graph (ROADMAP 5.1)

**Status:** Proposed. First item of Horizon 5 (`docs/ROADMAP.md`), and the only
one that needs no model on any path. Worth shipping even if nothing after it
ever is: it retroactively fixes provenance walks for features already shipped
(1.2's supersession chain, 1.3's inspector).

**Scope:** Make the relationship graph *interrogable* — filter a traversal by
edge kind, walk it backwards, and see which edge reached each hop. Nothing
about facts, rules, or inference; those are 5.2/5.3 and this design
deliberately does not anticipate them beyond not blocking them.

Relationships have existed since Phase 4. They are directed, carry a free-text
`kind`, are idempotent per `(to_id, kind)`, and are stored *inside* the record
(`record.h:11`). Two kinds are already written in anger: `supersedes` by
consolidation (`qe_maint.c:441`) and `derived_from`. Neither can be queried as
what it is.

## 1. Goals & non-goals

**Goals**

- **Filter by kind.** `traverse` follows only the edge kinds asked for.
- **Walk backwards.** Answer "what supersedes this?" and "what was derived from
  this?" without scanning the corpus.
- **Legible paths.** Each hop reports the edge that reached it, so a returned
  set is a *path*, not an unordered bag whose shape must be inferred.
- **Stay in the grain.** One more derived, in-RAM, never-checkpointed index in
  the shape of `tag_index`/`lexical_index`. No new persistence, no new file, no
  new encryption surface, no new lock.
- **Opt-out, degrade to today.** `--no-edge-index` disables the RAM cost; the
  default `traverse` behaviour is byte-for-byte what it is now.

**Non-goals**

- **Not a graph query language.** Two new fields on `traverse`. No path
  expressions, no pattern matching (that is 5.2's `pattern`, and it is a
  `search` filter, not a language).
- **Not a forward-edge index.** Deliberate asymmetry — see §3.
- **Not a fix for in-record edge storage.** `MAX_RELATIONSHIPS` (4096,
  `qe_internal.h:12`) and the write amplification of rewriting a record per
  `relate` both stay. 5.1 does not need them fixed; 5.2 cannot avoid them.
- **Not weighted or attributed edges.** `kind` stays a string. No properties.
- **Not referential integrity.** A tombstoned record leaves dangling forward
  edges in its peers' records, as today. See §5.4 and §12.

## 2. What exists today, precisely

`qe_traverse` (`query_engine.c:627`) is a depth-bounded BFS. Per level it:

1. takes `index_lock` for read, resolves each not-yet-seen frontier id to a log
   offset via `hash_index_get`;
2. takes `log_lock` for read and drops `index_lock` (so disk I/O never runs
   under the index lock);
3. reads and decodes each record, accumulates it into the result, and enqueues
   **every** outgoing neighbour — `next[next_n++] = r.relationships[k].to_id`
   (`query_engine.c:741`), with no reference to `kind`.

So: no kind filter, no reverse direction, no record of which edge was taken.
"Show me the supersession chain" is a blind BFS plus client-side filtering, and
"what depends on this?" has no answer short of decoding every live record.

The gap is not uniform, and that is what makes this cheap:

| Capability | Needs new state? |
|---|---|
| Filter by `kind` | **No** — `kind` is already in the decoded record |
| `direction: out` | **No** — forward edges are already in the record |
| Report the edge per hop | **No** — same |
| `direction: in`/`both` | **Yes** — nothing maps `to_id` → sources |

## 3. Design: two independent halves

**Half A — filtering (no new state).** Forward traversal already decodes each
frontier record, and the edges with their kinds are right there. Kind filtering
and per-hop attribution are a predicate and two extra fields inside the existing
enqueue loop. Zero RAM, zero maintenance, no flag, nothing to rebuild or
invalidate. This half ships first and alone.

**Half B — reverse adjacency (the only part that costs anything).** A reverse
index `to_id → [(from_id, kind)]`, maintained on write.

### Why not index forward edges too

The record *is* the forward adjacency list, and traverse already pays to decode
it. A forward index would duplicate durable state in RAM to save nothing: the
log read happens regardless, because the walk must return the record's contents,
not just its id. The asymmetry is deliberate and should be commented at the
structure definition, or someone will "fix" it.

### Alternatives considered for Half B

- **No index; scan on demand.** Answer a reverse query by walking `hash` and
  decoding every live record. Correct, zero RAM, and O(corpus) log reads per
  query — at 100k records, 100k reads for something the inspector calls
  interactively while a human waits. Rejected.
- **Lazy build, cached, invalidated on write.** Saves RAM only for deployments
  that never traverse backwards, which `--no-edge-index` already serves without
  an invalidation path to get wrong. Writes are serialized under `index_lock`
  anyway, so eager maintenance is nearly free. Rejected as complexity for no
  measurable gain.
- **Persist it.** Nothing to gain: it is derivable from the log in the same
  pass that rebuilds tag/time/lexical, and a checkpoint would add a file, a
  crypto surface (`ckpt_crypt`), and a staleness class. Rejected.

## 4. Structure & memory

Two pieces, mirroring `tag_index`'s bucket-chain-plus-sorted-postings shape.

**Kind interning.** A string → `uint16_t` table. Edge kinds are a low-cardinality
vocabulary by nature (`supersedes`, `derived_from`, …), so interning turns every
per-edge kind comparison into an integer compare and keeps the postings
fixed-width. A client can nonetheless send a unique `kind` per edge, so the table
is capped (`EDGE_MAX_KINDS`, 4096) and so is a single kind's length
(`EDGE_MAX_KIND_LEN`, 64). Past either limit the edge is **still indexed**; only
its label is lost, recorded as `kind_unknown`, and such an edge is returned by
*every* kind filter as a candidate for the caller to confirm against the record.
Completeness before precision: dropping the edge would make "what depends on
this?" answer wrongly, whereas an imprecise filter that says so is merely
unaccelerated. An over-long kind is likewise flagged rather than truncated —
truncating would silently collapse two distinct kinds into one and answer the
filter *wrongly*. Interned strings are allocated once and never moved or freed
while the index lives, so a returned `kind` pointer stays valid past the lock it
was read under — which is what lets a traversal carry it across the
`index_lock` → `log_lock` handoff.

**Reverse adjacency.** `to_id` → a sorted array of `(from_id, kind_id)`
postings, in an open-addressed table keyed by `to_id` (the lexical index's
doc-table shape: power-of-two, linear probing, grown at a 3/4 load factor, with
tombstones dropped on rehash). String-keyed structures get `tag_index`'s chained
buckets; this one is id-keyed and scales with the corpus, so it grows. Postings
are sorted so insertion is dedup-checked (matching `relate`'s existing
idempotency) and removal is a binary search rather than a scan.

**Sizing — measured, not estimated.** A posting is 16 bytes (8-byte `from_id`,
interned kind, alignment). But the per-edge figure is dominated by the *target*
table, not the postings, so it depends entirely on fan-in:

| Shape | Bytes/edge |
|---|---|
| 1 source per target (a `supersedes` chain) | ~121 |
| 2 sources per target | ~69 |
| 4 sources per target | ~42 |
| 1000 sources per target (a hub) | ~17 |

So 1M edges of the sparsest, most provenance-like shape costs ~120 MB, and the
same edges concentrated on fewer targets cost a fraction of that. That is a
larger number than a naive per-posting estimate suggests, and the reason is
worth stating: a target slot is ~40 bytes and the table runs at a 3/4 load
factor, so a target holding *one* incoming edge pays ~107 bytes of table for 16
bytes of payload. Reported in `stats` and counted toward `--max-index-bytes`
like every other index (§9), so it is watchable rather than a surprise.

```c
typedef struct EdgeIndex EdgeIndex;

/* One source pointing at the queried record. `kind` is an interned string, or
 * NULL for an edge stored without one; `kind_unknown` says the label could not
 * be interned, so `kind` is unknown rather than absent. */
typedef struct {
    uint64_t from_id;
    const char *kind;
    int kind_unknown;
} EdgeSource;

EdgeIndex *edge_index_create(void);
void edge_index_free(EdgeIndex *e);

/* All of these tolerate a NULL index as "no edge index configured", so
 * write-path call sites stay unguarded — the lexical_index convention. */
int edge_index_add(EdgeIndex *e, uint64_t from_id, uint64_t to_id,
                   const char *kind);
void edge_index_remove(EdgeIndex *e, uint64_t from_id, uint64_t to_id,
                       const char *kind);
/* Drop every edge with `id` as its *target*. O(indegree), no scan. */
void edge_index_remove_target(EdgeIndex *e, uint64_t id);

/* Sources pointing at `to_id`, optionally restricted to `kinds`. Allocates *out
 * (free with free()). Sorted by from_id ascending; the order among edges
 * sharing a from_id is internal and not contractual. Returns 0/-1. */
int edge_index_sources(const EdgeIndex *e, uint64_t to_id,
                       const char *const *kinds, size_t n_kinds,
                       EdgeSource **out, size_t *out_n);

size_t edge_index_edges(const EdgeIndex *e);
size_t edge_index_kinds(const EdgeIndex *e);
size_t edge_index_bytes(const EdgeIndex *e);
```

Not internally synchronized; callers hold `db->index_lock` — the `tag_index` and
`lexical_index` convention.

## 5. Index maintenance

Six sites. Five are the ones every derived index already touches; one is new.

### 5.1 `qe_relate` — the new site

`qe_relate` (`query_engine.c:556`) currently touches no secondary index, because
edges were not indexed. It becomes the primary maintenance point: after
`append_and_hash` succeeds, `edge_index_add(db->edges, from_id, to_id, kind)`,
still under the write lock it already holds. Its existing idempotency check
means a duplicate edge returns before reaching the index.

### 5.2 `qe_insert` — defensive no-op

`insert` never carries relationships (they arrive only via `relate`), so the
site at `query_engine.c:304` indexes whatever edges the record happens to hold
— today, none. Included so the insert path stays uniform with the replica path
(§5.5), which *does* receive edges, rather than encoding "inserts have no edges"
as an invariant that a future `promote` or batch path can quietly break.

### 5.3 `qe_update` — no-op

An update patch cannot change relationships (tags and payload only, hence the
existing tag/lexical swings at `query_engine.c:435-445`). No edge work. Worth a
one-line comment saying so, so its absence reads as intentional.

### 5.4 `qe_delete` — both directions

At `query_engine.c:487`, alongside the existing tag/lexical/usage/semantic
removals:

- **outgoing**: `edge_index_remove` per entry of `cur.relationships` — the
  record being tombstoned is in hand, so its edges are known.
- **incoming**: `edge_index_remove_target(db->edges, cur.id)` — O(indegree)
  precisely *because* the reverse index exists. Without it this would be a
  corpus scan, which is the second reason Half B pays for itself.

The peers' records still list the tombstoned id, and are **not** rewritten. That
matches the existing discipline (a tombstone never touches other records) and
forward traversal already skips a target whose `hash_index_get` is absent or
deleted. The invariant to state and test: *the edge index never reports an edge
whose endpoint is not live; a record's own `relationships` array may.* See §12.

### 5.5 `db_replica_apply` — the real diff site

`db.c:238`/`db.c:261` diff a record's prior version out of the secondary indexes
and add the new one in. This is the one place edges arrive *with* a record, since
replication ships whole records — so unlike §5.2 this is load-bearing. Remove
the prior version's edges, add the new version's.

### 5.6 `recovery.c` — full rebuild

At `recovery.c:155-161`, alongside time/tag/lexical: index each live record's
edges. Derived and never checkpointed, so it is always rebuilt in full — the
lexical-index precedent, and the reason no `edge.db` exists.

Consolidation (`qe_maint.c:409-441`) needs no changes: it re-relates through
`qe_relate` and tombstones through `qe_delete`, so it is covered transitively.

## 6. Wire protocol

`traverse` gains two optional fields:

| Field | Type | Notes |
|---|---|---|
| `kinds` | string[] | Follow only these edge kinds; absent = all (today's behaviour) |
| `direction` | string | `out` (default) \| `in` \| `both` |

`direction` defaults to `out`, so an existing client sees no change. `in`/`both`
against a server started with `--no-edge-index` returns `NOT_READY` — exactly
how `search`'s `query` behaves under `--no-lexical-index`.

Each returned record gains a `traversal` object describing **how BFS first
reached it**:

```json
{ "traversal": { "depth": 2, "via_id": 42, "via_kind": "supersedes",
                 "via_direction": "out" } }
```

The start record reports `depth: 0` with the other fields absent. `via_direction`
matters only for `both`, where an id can be reachable either way and the pair
`(via_id, via_kind)` alone would be ambiguous about orientation.

Namespace scoping is unchanged and applies to reverse edges identically: a
namespaced caller must own a record for it to appear, so a reverse walk cannot
be used to discover that a co-tenant links to your record.

## 7. Locking & concurrency

No new lock, and no change to the existing order (`index_lock` → `log_lock`).

The BFS loop shape does change, because the two directions are resolved in
different phases:

- **Reverse neighbours** come from the edge index, so they are gathered in the
  resolve phase, under `index_lock` for read — where the walk already is.
- **Forward neighbours** come from the decoded record, so they continue to be
  gathered in the read phase, under `log_lock`.

Both feed the same `next` frontier, which is then deduplicated against `seen` as
it is today. The existing invariants hold: no allocation under `log_lock` beyond
what already happens there, no lock upgrade, and no disk I/O under `index_lock`.

Writes take `index_lock` for write already at every one of §5's sites, so edge
maintenance adds no lock acquisition anywhere.

## 8. Compaction, replication, encryption

- **Compaction** is a non-event. It rewrites the log and rebuilds `hash` because
  *offsets* move; the edge index keys on ids, exactly like tag/time/lexical, so
  it needs no rebuild and no generation bump.
- **Replication** is covered by §5.5. A replica builds the same index from the
  records it applies, so a reverse query answers identically on primary and
  replica — worth a contract test (§10).
- **Encryption** is untouched: nothing new is persisted.

## 9. Observability

`stats` reports `edge_kinds`, `edges`, and `edge_bytes`, and `edge_bytes` joins
the `index_bytes` total that `--max-index-bytes` enforces. The Prometheus
exporter and the Grafana index-RAM panel pick these up the way they did
`lexical_*` in 4.1 — a new index that is not in the RAM panel is a new way to
run out of memory silently.

## 10. Testing

**Unit** (`tests/unit/test_edge_index.c`, modelled on
`tests/unit/test_lexical_index.c`): add/remove/dedup, sorted-postings
invariants, `remove_target`, kind interning and the `EDGE_MAX_KINDS` fallback,
byte-accounting monotonicity, NULL-index tolerance at every entry point.

**Contract** (`tests/contract/test_wire_protocol.py`):

1. A consolidated record's supersession chain is retrieved backwards in **one**
   `traverse` call — the ROADMAP 5.1 "done when".
2. An edge kind not named in `kinds` is **not** followed (the regression this
   exists to prevent).
3. `direction: "in"` under `--no-edge-index` → `NOT_READY`.
4. `delete` drops the record from both directions: it no longer appears as a
   reverse source, and a forward walk into it stops.
5. Restart rebuilds the index — a reverse query answers identically before and
   after (recovery, §5.6).
6. A replica answers the same reverse query as its primary (§5.5).
7. A namespaced token cannot see a co-tenant's record via a reverse walk.
8. `traversal.via_kind`/`via_id` name a real edge for every non-start hop.

No `make eval` work: 5.1 changes what can be *asked*, not what ranks. The eval
harness enters at 5.3, where inference has recall consequences.

## 11. Rollout (PR sequence)

1. **Half A** — `kinds` + `direction: "out"` + the `traversal` object in
   `qe_traverse`/`handle_traverse`, plus wire-protocol docs and contract tests
   2 and 8 (test 1 needs the reverse walk, so it lands with PR 4). No new state,
   no flag, no RAM. Independently useful: it is what the inspector's provenance
   tree needs.
2. **`edge_index.h/.c`** + unit tests, unwired. Reviewable in isolation.
3. **Wiring** — the six sites of §5, `--no-edge-index`, recovery rebuild, stats
   and exporter (§9). Still no reader, so a bug here is invisible to clients and
   caught by tests 5–6.
4. **`direction: "in"|"both"`** — the reverse reader, the resolve-phase frontier
   change (§7), and contract tests 3–7.
5. *(optional)* Inspector: render the provenance tree Half A made possible.

Half A is shippable on its own and answers a real question today. If Horizon 5
stops after PR 1, the tree is still better off.

## 12. Open questions

- ~~**`EDGE_MAX_KINDS` and the fallback.**~~ **Decided (PR 2):** the fallback,
  and for a firmer reason than the original "leaning weakly". The alternative —
  refusing to index an edge whose kind will not intern — makes reverse results
  *incomplete*, and an incomplete answer to "what depends on this?" is a
  correctness bug, whereas an imprecise-but-labelled one is a caller-side
  confirmation the caller was going to do anyway (it is already reading the
  record). Same argument settles over-long kinds: flag, never truncate.
- **Dangling forward edges (§5.4).** Compaction already rewrites live records,
  so it *could* prune edges whose target is gone — cheap, and it would make the
  record agree with the index. But compaction currently only relocates bytes and
  never alters record contents, and breaking that property to tidy an
  inconsistency that no reader observes seems like a bad trade. Leaving them.
- **`search` with a graph filter.** Once the index exists, a
  `related_to: {id, kind, direction}` filter on `search` is nearly free and
  obviously useful ("memories derived from this one, ranked by relevance").
  Deliberately deferred: it is the first step toward 5.2's `pattern`, and
  bundling it here would make 5.1 the thin end of a query language. Revisit when
  5.2 lands, not before.
- **Shrinking the sparse case.** The table above says the cost is per *target*,
  not per edge, and that a single-incoming-edge target is the expensive shape —
  which is also the common one. Two levers, neither taken in PR 2: store the
  first posting *inline* in the target slot (removing both the separate
  allocation and its slack for 1-in targets, at the price of a branch on every
  posting access), or narrow the slot's `n`/`cap` from `size_t` to `uint32_t`
  (40 → 32 bytes, ~20% off the dominant term, at the price of a bound that is
  unreachable in practice but silent if ever reached). Worth doing only if
  `edge_bytes` in a real deployment says so — which §9 makes visible.
- **When to move edges out of the record.** In-record storage caps a node at
  `MAX_RELATIONSHIPS` and rewrites the whole record per `relate`. 5.1 does not
  need this changed and should not change it. 5.2's typed facts will force the
  question; the answer is probably an edge record type with its own log frames,
  and it deserves its own design doc rather than a paragraph here.