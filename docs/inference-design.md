# Design: Deterministic Inference & Truth Maintenance (ROADMAP 5.3)

**Status:** Proposed. Third item of Horizon 5, and the first one that *writes
records the client did not send* — 5.1 derived an index, 5.2 derived nothing at
all.

**Scope:** Draw the conclusions the 5.2 registry already declares are available
(`transitive`, `symmetric`, `inverse_of`), answer a question about a category
with a memory about one of its members, catch a contradiction that a declared
`cardinality: one` or `mutex_with` makes deterministic, and — the half that
makes the rest safe — retract a conclusion when its premise goes away. **No
model calls, no rule language, no search.** Every conclusion here follows from a
declaration in a config file and two facts already in the log.

## 1. Goals & non-goals

**Goals**

- **A conclusion is a record.** Not an index entry, not a query-time flourish.
  It is durable, replicated, tombstonable, and carries links to the premises it
  came from, so `explain` can show a derivation rather than a similarity score.
- **Retraction is not optional.** Tombstoning a premise today leaves its
  consequences standing and nothing notices. A system that derives without
  retracting is worse than one that never derived — it manufactures claims that
  outlive their reasons. Retraction ships in the same horizon as derivation,
  not after it.
- **Determinism.** The same log, the same registry, the same conclusions. No
  embedding, no threshold, no sampling. This is the property that makes an
  inference auditable, and it is the whole reason to do this in C in a database
  rather than in a prompt.
- **Off the write path.** `insert` latency must not depend on how many rules
  could fire. The precedent is already in `compaction.c`'s maintenance tick —
  the TTL sweep, the checkpoint, the off-lock HNSW build: bounded work on a
  timer, each independently disableable.
- **Invisible when unconfigured.** With no registry — the default — no
  predicate declares anything, so nothing is derivable and the job has nothing
  to do. A deployment that never opted into a vocabulary sees no new records, no
  new RAM, and no new log growth.

**Non-goals**

- **Not a rule engine.** No user-authored rules, no Datalog, no
  negation-as-failure, no unrestricted recursion. Three fixed inference shapes,
  driven by five flags the registry already has. If a deployment needs a fourth,
  that is a code change and a design conversation, which is the correct friction
  for something that writes records on its own.
- **Not defeasible reasoning.** "Normally X, except here" stays deferred, per
  the roadmap. Supersession covers the common case and non-monotonic logic has a
  poor ratio of value to subtlety.
- **Not resolution.** A contradiction is *reported*, never silently resolved.
  Choosing which of two conflicting facts survives is a judgment, and this layer
  does not make judgments — that is what `supersedes` and a human or a model
  are for.
- **Not entity resolution.** Still 5.4. Two records denoting the same thing
  stay two things here, which is precisely why the `is_a` taxonomy has to be
  authored rather than inferred.
- **Not a new query surface.** `pattern` gains reach (§4.2) but not syntax. No
  variables, no joins — the 5.2 line holds.

## 2. What exists today

5.1 gave the graph a reverse direction (`edge_index`) and made `traverse`
selective by `kind`. 5.2 gave a record an optional `{s, p, o}` triple, three
indexes that answer any non-empty subset of it, and a registry that declares —
and validates, but does not act on — `cardinality`, `symmetric`, `transitive`,
`inverse_of` and `mutex_with`.

So the ingredients are all present and all inert. A registry can say `part_of`
is transitive, and the server will refuse a fact that misuses it and then draw
no conclusion from it. `explain` exists but explains *ranking* — which of
similarity, BM25, RRF and recency put a record where it is. It has no vocabulary
for "you believe this because of these two facts and this rule."

Three existing behaviours constrain everything below, and are worth stating
before the design leans on them:

- **A tombstone never rewrites its peers.** `qe_delete` drops the deleted
  record's own edges and every edge *pointing at* it (`edge_index_remove_target`),
  but leaves the peers' `relationships` arrays naming it. The invariant is that
  the edge index never reports an edge whose endpoint is not live.
- **The maintenance thread skips write work on a replica.** `compaction.c`'s
  tick does fsync and the semantic build everywhere, and sweep/compaction only
  when `!read_only`, because a follower appending frames its primary never sent
  desyncs the byte-identical log.
- **A fact is immutable.** `update` refuses one. Changing what a record asserts
  is a supersession.

## 3. Where a conclusion lives: codec v4

A derived fact is a `MemoryRecord` like any other — a `semantic` record with a
`fact`, a payload, and `derived_from` relationships to each premise. It goes
through the ordinary write path, so it replicates, compacts, tombstones, obeys
namespaces, and appears in `search` with no new cases anywhere.

What it needs that no existing field provides is **the rule that fired**. The
roadmap asks for it explicitly, and without it a derivation tree can show which
records were involved but not why they imply anything.

Codec **v4** adds an optional `Derivation`:

```
derivation := { route_count: u16,
                routes: [ { rule: u8, depth: u16,
                            premise_count: u8, premises: u64[] } ] }
```

A conclusion carries **every** justification it has, not one. Support is
disjunctive — the conclusion stands while any single route's premises are all
live — so a flattened premise list cannot answer the question retraction has to
ask. With one list, losing any premise looks fatal and losing all of them looks
survivable; both are wrong when two independent chains reach the same triple.

Encoded between the fact and the payload, so the variable-length payload stays
last as it always has. **A derivation requires a fact**: every rule here
concludes a triple, so a derivation without one would be provenance for
nothing, and `record_encode` refuses it rather than putting an uninterpretable
frame in the log. A rule byte the build cannot name is refused on decode —
unlike an unknown `FactKind` the framing is fixed and could be skipped, but
provenance nobody can read is the one thing this field exists to prevent.

and the same compatibility rule that governed v3 governs this: **`record_encode`
emits the lowest version that represents the record.** A record with no
derivation still encodes as v3 if it has a fact and v2 if it does not, so a
deployment that never enables inference is unchanged on disk and on the wire,
and the first v4 frame appears exactly when someone turns the job on. The
replication codec gate shipped in 5.2 already handles the rest: a primary
writing v4 withholds those frames from a v3 replica and says why.

`premises` duplicates what the `derived_from` edges say, deliberately. The edges
are what a *walk* uses; the array is what survives the edges. When a premise is
tombstoned its incoming edge leaves the index (§2), so a derived record that
kept its lineage only in the graph would lose the ability to explain itself at
exactly the moment the explanation matters most — "this was derived from
something that has since been retracted" is the single most useful thing
`explain` can say. Eight bytes per premise, capped at `DERIV_MAX_PREMISES`, is a
cheap price for a record that stays self-describing after its context is gone.
(This said "sixteen bytes" before the codec was written; a premise is one u64.)

The payload is synthesized, because `insert` refuses an empty one and a derived
record should be readable by a human who finds it in a `search` result: *"hnsw.c
is part of the storage layer (derived: transitive part_of, from #88, #91)."*
That also makes conclusions findable lexically, which costs nothing — BM25 is
already indexing every payload — and gives the eval dataset a way to confirm
that the *symbolic* path is what answered, not the words.

**`derivation` is server-only.** `insert` rejects a request carrying one with
`INVALID_REQUEST`, the same way `update` rejects a `fact`. A client that could
forge a derivation could manufacture provenance, which is worse than having none
— every trust claim in this horizon rests on the field being unforgeable. The
replica path does not check, for the reason it does not check the registry: it
applies what the primary already accepted.

**Rejected: an index-only closure.** Keeping derived facts in RAM and rebuilding
them at startup costs no log growth and needs no codec bump, and for the
*retrieval* benefit alone it would be enough. It fails the other two goals
completely: there is nothing durable for `explain` to point at, and nothing to
tombstone when a premise is retracted — the closure would simply be recomputed
without the premise on the next restart, silently, with no record that a belief
changed. The audit trail is the product here.

**Rejected: a tag.** `derived` plus `rule_transitive` as tags needs no format
change at all. But tags are client-writable, so provenance would be forgeable;
they are also a flat set with no room for the premise list or the depth. Cheap
and wrong.

## 4. The three inferences

### 4.1 Transitive, symmetric, inverse — materialized

These produce facts that are *new and true*, so they become records.

| Declaration | Given | Derive |
|---|---|---|
| `"transitive": true` | `(a p b)`, `(b p c)` | `(a p c)` |
| `"symmetric": true` | `(a p b)` | `(b p a)` |
| `"inverse_of": "q"` | `(a p b)` | `(b q a)` |

All three are restricted to `object: "id"` predicates, which 5.2 already
enforces at registry load: each relates a subject to a subject, and subjects are
records.

Each candidate conclusion is checked against the fact index before it is
written. This is the payoff from 5.2 that makes the whole job affordable: "does
`(a p c)` already exist?" is a `{s, p, o}` lookup, not a scan, so re-running the
job over a quiet corpus writes nothing. Idempotence is not a special case; it is
the normal case.

It is *not*, however, free. An earlier draft of this section said a quiet pass
costs "O(facts) index probes"; implementing it showed the cost is O(candidates),
which for a transitive predicate is O(facts × fan-out) — and on a corpus whose
closure is already materialized every one of those candidates is a duplicate.
Measured on a fully closed chain, a pass went cubic in chain length: 0.5 s at
400 nodes, 3.7 s at 800, 12.9 s at 1200, producing nothing each time. A cap on
conclusions *written* never fires there, so it bounds the wrong thing. §5 has
the cap that bounds the right one.

**Cycles are the obvious hazard** and the fact-index dedup handles them without
a cycle detector: `a part_of b`, `b part_of a` derives `a part_of a` and
`b part_of b` once each, and then every subsequent pass finds them already
present and stops. A self-edge is a strange fact but a harmless one, and
refusing it would cost a reachability check on every derivation to prevent
something the dedup already bounds. Depth and fanout caps (§5) bound the rest.

### 4.2 Subsumption is a query-time expansion, not a closure

The roadmap lists subsumption over `is_a` as the first of three closures. **On
implementation it should not be one**, and this is the largest revision this
design makes to the entry.

The goal — "a memory about `hnsw.c:214` answers a question about the storage
layer" — describes broadening a *question*, not deriving a *fact*. Materializing
it means writing, for every fact about `hnsw.c` and every ancestor of `hnsw.c`,
a copy of that fact with the ancestor as subject. That is wrong twice over: it
is false (the storage layer does not default to what `hnsw.c` defaults to), and
it is quadratic in taxonomy depth × facts per entity, which is the one shape in
this design that could actually run away.

Instead, `pattern` gains subsumption on the subject position:

```json
{ "operation": "search", "pattern": { "s": 42, "p": "defaults_to" },
  "subsume": true }
```

With `subsume` set, a bound `s` matches the subject *or any record that reaches
it through `is_a`*. The descendants come from one fact-index lookup —
`{p: "is_a", o: {id: 42}}` — because `is_a` is declared transitive and §4.1 has
already materialized its closure. So the two mechanisms compose: the closure
that *is* worth materializing turns the expansion that is not into a single
index probe rather than a graph walk.

Opt-in, and off by default: it changes what a pattern means, and a caller asking
"what does record 42 default to?" should not silently get an answer about a
different record. The expansion set is capped (`--inference-max-subsume`,
default 256 descendants); past the cap the search returns what it has and
reports truncation in `explain`, rather than quietly narrowing.

This keeps `pattern` a filter. It gains no variables and no joins — one boolean
that says which subjects count as the subject.

### 4.3 Contradiction detection

Two sources, both declared, both deterministic:

- **`cardinality: "one"`** — two live facts share `(s, p)` with different `o`.
- **`mutex_with: ["q", …]`** — a live `(s, p, o)` and a live `(s, q, o')` where
  the registry says `p` and `q` cannot both hold of a subject.

Detection is a scan of the subject index per pass, bounded the same way
everything else here is. A contradiction is **reported, never resolved**: the
job emits a `conflicts_with` edge in *both* directions between the two records,
increments a `conflicts` stat, and logs once at `WARN` naming both ids and the
predicate. Nothing is tombstoned, nothing is reranked, no record is marked
untrustworthy.

That restraint is the design. Deciding which of two conflicting facts is right
requires knowing which is newer, which source is better, or what the world is
actually like — none of which this layer knows. What it can do is guarantee the
contradiction is *found*, immediately and without a model call, which is the
thing a human or a 5.4 extractor cannot do reliably on its own.

Conflicts are also the one output here that is not a derived record. They are a
relation between two existing records, so they need no premises and no rule
provenance — the two endpoints *are* the explanation.

## 5. The inference job

A step on the existing maintenance tick, next to `db_semantic_build_step`:

```c
db_inference_step(db);   /* every --inference-interval-sec, default 30 */
```

**Primary only.** Skipped when `config.read_only`, for the reason sweep and
compaction are skipped: a follower that derived locally would append frames its
primary never sent and desync a log that must stay byte-identical. Derived
records reach a replica the way every other record does — through the stream. A
replica therefore holds exactly the conclusions its primary drew, which is also
the only way the two can be compared.

**Bounded per pass**, by three caps, all configurable:

| Cap | Default | Bounds |
|---|---|---|
| `--inference-max-candidates` | 1,000,000 | conclusions *considered* per pass |
| `--inference-max-depth` | 4 | derivation chain length |
| `--inference-max-derived` | 1000 | records *written* per pass |
| `--inference-max-subsume` | 256 | descendants in a `subsume` expansion |

The candidate cap is the one that actually bounds a tick, and it exists because
the other two do not: depth bounds chain length, and `max-derived` bounds
writes, but a closed corpus does unbounded work while writing nothing (§4.1).
A budgeted pass must also start where the last one stopped, or it examines the
same prefix forever and never reaches the rest — so the pass takes a rotating
start offset and reports how many candidates it got through.

Hitting a cap is not an error and not silent: the pass stops, logs what it
deferred, and the next tick continues. The alternative — running to fixpoint —
makes one tick's duration a function of corpus shape, which is how a background
job becomes an outage.

**The pass, per namespace** (see §7): read the candidate facts under
`index_lock` (read), compute conclusions off the lock, then write each through
the ordinary `qe_insert` + `qe_relate` path. Writing off the read lock means the
fact set can change underneath the computation — a premise may be tombstoned
between the read and the write — and `qe_relate` behaves differently in the two
auth modes when that happens. A namespaced caller gets `NOT_FOUND`, because the
target is loaded and its `deleted` flag checked; an unnamespaced one succeeds,
because the check is only `hash_index_get`, and a tombstone still has a hash
entry.

Neither outcome needs preventing, and that is the point of storing the premise
ids in the record (§3) rather than only in the edges: the conclusion is durable
and self-describing whether or not its edges attached, and the reconciliation in
§6 retracts it on the next tick either way. Same eventual state, no write lock
held across a batch, and no need to make the pass atomic with respect to
deletes.

`depth` comes from the premises: a conclusion is one deeper than its deepest
premise, and a premise with no derivation is depth 0. So the cap is enforced by
reading the field the last pass wrote, with no traversal.

## 6. Truth maintenance

### The ordering problem

The roadmap says retraction "falls out of 5.1's reverse adjacency: retract a
premise, walk `derived_from` backwards, tombstone the dependents." It does not,
quite, and the reason is the tombstone invariant from §2.

`qe_delete` calls `edge_index_remove_target(db->edges, cur.id)` — the incoming
edges are gone by the time the delete returns. A background job that walks
`derived_from` backwards from a retracted premise on the *next* tick finds
nothing. The dependents' own `relationships` arrays still name the premise, so
the information survives in the log, but finding them from there is a corpus
scan, which is exactly what the edge index exists to avoid.

This is the same shape as the caveat `symbolic-layer-design.md` already records
about `consolidate`: the `supersedes` edge leaves the reverse index the moment
it is created, because its target is tombstoned in the same breath.

### Support is disjunctive

A conclusion is retracted when **every** route it carries has lost a premise,
not when the first one does. Two independent chains reaching the same triple
each justify it on their own, so retracting on the first broken route would
tombstone a record the log still fully supports — and the next pass would
re-derive it under a new id, so the conclusion would flap rather than settle.
That is what §3's route set is for: the check is "does any route survive?",
which a flat premise list cannot express.

### Capture under the lock, retract off it

`qe_delete` already holds `index_lock` for write and the reverse index is still
intact at that point. So it reads the dependents it is about to orphan —
O(indegree) on an index that exists for this — and pushes their ids onto a
**retraction queue** before `edge_index_remove_target` runs:

```c
/* before the index teardown, while the reverse edges still exist */
db_retract_enqueue(db, cur.id);   /* sources of derived_from -> cur.id */
```

The maintenance tick drains the queue, tombstoning each dependent through the
ordinary delete path — which enqueues *its* dependents in turn, so the cascade
proceeds breadth-first across ticks and is bounded by the same per-pass cap as
derivation. No recursion under a write lock, no unbounded work in a client's
`delete` call.

### The queue does not need to be durable

A crash with a non-empty queue would leave conclusions outliving their premises
— the precise failure this section exists to prevent. The fix is not to persist
the queue but to make it **reconstructible**, and it already is: a derived
record names its premises in the `derivation.premises` array (§3), so recovery,
in the pass that already walks the live set, can check each derived record's
premises against the hash index and enqueue any whose premise is absent or
tombstoned.

That turns the queue into a pure optimization — the fast path for the common
case, with a full reconciliation on every restart as the backstop. It also
means the invariant is *checkable*: after recovery, no live derived record has a
dead premise. That is a much better property to be able to assert than "the
queue was flushed."

This is also the second time the premise array earns its 16 bytes, and the
reason it is stored rather than read back from the edges.

### What retraction is not

Retracting a derived record **tombstones** it; it does not delete its
provenance. The tombstone is a log entry like any other, `history` still shows
the record and its derivation, and `as_of` still reconstructs the belief state
at a past time. "We believed this, because of that, until the premise was
retracted at T" is recoverable, which is the point of doing this in an
append-only log.

An **asserted** record is never retracted by this machinery, only derived ones.
A human-supplied fact whose premise disappears is not a contradiction; it is
just a fact.

## 7. Namespaces

**A rule must never join facts across tenants.** The fact indexes are
server-wide — 5.2 made them so deliberately, with `passes_filters` re-checking
`agent_id` on the loaded record — which means the naive implementation of §4.1
would happily read `(a p b)` from tenant A and `(b p c)` from tenant B and write
`(a p c)`, a record that exists in neither tenant's world and leaks the
existence of both premises.

So the pass runs **per namespace**: candidates are drawn with the tenant filter
applied, and a conclusion inherits the `agent_id` of its premises, which are all
from that namespace by construction. A conclusion whose premises somehow
disagree on `agent_id` is a bug, and is refused with a `WARN` rather than
written under one of them.

With auth off there are no namespaces and the whole question is moot, which is
the common single-user case.

This is the one part of the design where a mistake is a data leak rather than a
wrong answer, so §12 gives it a dedicated test rather than folding it into the
derivation tests.

## 8. Confidence

A derived record's confidence is the **product of its premises' confidences,
with a floor**:

```
confidence = max(product(premise confidences), --inference-confidence-floor)
```

Default floor 0.1. The product is the honest shape — a chain is at most as
trustworthy as its weakest link, and independent-ish premises should compound —
and the floor keeps a depth-4 chain of 0.7s from decaying to noise and vanishing
from ranked results.

This is **a heuristic, not a probability**, and the doc says so out loud because
the number is going to end up in a ranking function where it will look like one.
The premises are not independent, the product has no calibration, and nothing
here is Bayesian. What it *is* is monotonic (a derived fact never outranks its
premises), deterministic, and testable — three properties worth more here than
nominal correctness.

## 9. `explain` over a derivation

`explain` currently answers "why did this rank here?". It gains a second,
disjoint answer: "why do you believe this at all?".

```json
"explain": {
  "semantic": false, "lexical": false,
  "derivation": {
    "rule": "transitive", "depth": 2, "confidence": 0.49,
    "premises": [
      { "id": 88, "fact": {"s": 12, "p": "part_of", "o": {"id": 34}},
        "live": true },
      { "id": 91, "fact": {"s": 34, "p": "part_of", "o": {"id": 56}},
        "live": false }
    ]
  }
}
```

One level, not a tree. A premise that is itself derived carries its own
`derivation` when fetched, so a client walks the chain by following ids — the
same shape `traverse` already uses, and it keeps a single response from
expanding to the size of a derivation forest. `live` is what makes a
mid-retraction state legible instead of confusing.

## 10. Locking, recovery, replication, compaction

- **Locking.** No new lock. The job takes `index_lock` (read) to collect
  candidates and releases it to compute; each write takes it for write through
  the existing `qe_insert`. The retraction queue gets its own small mutex,
  ordered *below* `index_lock` (taken while `index_lock` is held in
  `qe_delete`, never the reverse), so no new cycle.
- **Recovery** reconciles derived records against their premises (§6) in the
  pass that already walks the live set. It derives nothing — recovery must be a
  function of the log, and running rules during it would make startup produce
  records that the log does not contain.
- **Replication.** Derived records ship as ordinary frames; the codec gate
  handles v4 the way it handles v3. A follower never derives and never retracts
  on its own. Primary/replica parity of derived state is therefore the same
  property as log parity, and testable as such.
- **Compaction** is unaffected. Derived records are records; `derivation`
  premise ids are record ids, not log offsets, so relocation is a non-event —
  the same reason the fact index survives compaction.

## 11. Config & observability

| Flag | Default | Effect |
|---|---|---|
| `--inference` | off | master switch; off means no job, no v4 frames |
| `--inference-interval-sec` | 30 | tick divisor for the pass |
| `--inference-max-candidates` | 1000000 | conclusions considered per pass |
| `--inference-max-depth` | 4 | chain length cap |
| `--inference-max-derived` | 1000 | records written per pass |
| `--inference-max-subsume` | 256 | descendants per `subsume` expansion |
| `--inference-confidence-floor` | 0.1 | floor on the product |

Off by default, unlike the 5.1 and 5.2 indexes, which are on. Those are derived
state that a client can only observe through a query; this one writes records
and grows the log, and a feature with that blast radius should be something an
operator turned on.

`stats` gains `derived`, `conflicts`, `retracted_total`, `inference_last_ms` and
`inference_deferred` (candidates a cap postponed). The last two are what tell an
operator whether the caps are sized right — a permanently non-zero
`inference_deferred` means the job never reaches fixpoint, which is survivable
but worth seeing.

## 12. Testing

Unit, per closure: transitivity including the self-edge cycle case; symmetry;
inverse pairs; the dedup path (a second pass writes nothing); depth attribution;
confidence product and floor; contradiction detection for both `cardinality`
and `mutex_with`.

Contract, end to end:

1. Declare `part_of` transitive, insert `a→b` and `b→c`, tick, and find
   `a part_of c` with a `derivation` naming both premises.
2. Re-tick. Nothing new is written (`derived` is unchanged).
3. Tombstone premise `a→b`. The derived record is tombstoned, and `history`
   still shows both it and its derivation.
4. **Restart with the retraction queue deliberately lost** (kill -9 between the
   premise delete and the next tick): recovery reconciles and the dependent is
   retracted anyway. This is the test that justifies §6's design, so it drives
   the crash rather than simulating it.
5. `subsume` finds a fact about a child when asked about the parent, and does
   **not** when `subsume` is absent.
6. Two facts violating `cardinality: one` produce `conflicts_with` edges both
   ways and a `conflicts` stat, and **neither record is tombstoned**.
7. A tenant's rule does not fire across a co-tenant's fact, and the conclusion
   carries the premises' `agent_id`. (Its own test, per §7.)
8. A replica holds exactly the derived records its primary does, and derives
   none of its own.
9. `insert` with a `derivation` is refused.
10. A v4 frame is withheld from a v3 replica with `MSG_INCOMPATIBLE`.

**`make eval` gains a multi-hop dataset**, which is the roadmap's "done when"
and the only claim here that retrieval quality is affected. Its queries must be
*structurally unanswerable* by retrieval: the answer lives in no single record,
so semantic, lexical and hybrid all score near zero on it while the symbolic
path answers. That asymmetry is the whole argument for this horizon, and if the
dataset does not show it, the dataset is wrong or the horizon is.

## 13. Rollout (PR sequence)

1. **Codec v4 + `Derivation`** — durable format, encode/decode, the golden test
   that a derivation-less record still encodes byte-identically, `insert`
   refusing a client-supplied derivation. No job, no rules. Riskiest and
   irreversible, so first, exactly as v3 was.
2. **The closures, pure and unwired** — `inference.h/.c` computing conclusions
   from a fact set and a registry, with no database, no threads, and no writes.
   Unit-testable in isolation; this is where the cycle and dedup behaviour gets
   pinned down.
3. **The job** — `db_inference_step`, the caps, the flags, per-namespace
   scoping, `stats`. Writes records for the first time. Contract tests 1, 2, 7.
4. **Truth maintenance** — the retraction queue, the `qe_delete` capture, the
   recovery reconciliation. Contract tests 3 and 4.
5. **Contradiction detection** — `conflicts_with`, the stat, the WARN.
   Contract test 6.
6. **`subsume` + `explain.derivation`** — the read-side surface. Contract
   tests 5 and 9, and the wire-protocol docs.
7. **The multi-hop eval dataset.**

PRs 1–4 are the spine: after 4 the system derives and retracts correctly, which
is the minimum honest version. 5 and 6 are independently useful and independently
revertible. If the horizon stops after 4, nothing is left in a half state.

## 14. Risks

- **Log growth.** Derived records are records. A dense taxonomy with several
  transitive predicates can multiply the corpus, and unlike an index this cannot
  be dropped and rebuilt — it is in the log forever, minus compaction. The caps
  bound the rate, not the total. `--inference` being off by default is the real
  mitigation, and `stats.derived` is the number to watch.
- **A wrong rule is durable.** A mis-declared `transitive` on a predicate that
  is not transitive writes false records that outlive the fixed registry. There
  is no un-derive; the remedy is `delete` on the derived set, which the
  `derivation.rule` field at least makes findable. Worth calling out in the
  operator docs.
- **The confidence number will be read as a probability.** §8 says it is not,
  in the code comment as well as here, because it will end up in a ranking
  function and look authoritative.
- **Routes are capped at `DERIV_MAX_ROUTES` (4).** A triple reachable more ways
  than that keeps the lowest-ordered routes. Dropping a route can only cost a
  conclusion a retraction and a re-derivation on the next pass, never a wrong
  answer, so the cap is a memory bound rather than a correctness one.
- **Cap tuning is deployment-specific** and a permanently deferred backlog is
  easy not to notice. Hence `inference_deferred` in `stats` rather than a log
  line nobody greps for.

## 15. Open questions

- **Should `subsume` also expand the object position?** `{p: "part_of",
  o: {id: storage_layer}}` arguably ought to find things that are part of a
  *descendant* of the storage layer. Symmetrical and cheap — the object index
  answers it the same way. Left out of this design only because the subject case
  is the one the roadmap motivates and the one the eval dataset will exercise;
  worth revisiting once there is a real taxonomy to test against rather than a
  synthetic one.
- **Where should the conflict live?** `conflicts_with` edges put it on the two
  records, which is where a reader looks. But an edge is not queryable in
  aggregate — "show me every contradiction" is a scan unless the stat is enough.
  A conflict *record* would be queryable and would carry its own provenance, at
  the cost of manufacturing a record for something that is arguably a relation.
  Deferred until someone actually wants the list.
- **Does `forget` understand derivation?** Decay-based forgetting scores records
  by age and usage. A derived record has no usage history of its own and will
  age out ahead of its premises, which is probably right (it is recomputable)
  but is currently an accident rather than a decision. Wants a look when 5.3 and
  2.3 are both in front of someone.
- **Interaction with `consolidate`.** Consolidation merges similar records and
  writes `supersedes`. If it absorbs a premise, the derived record's premise id
  points at a tombstone and §6 retracts the conclusion — even though the merged
  survivor still asserts the same thing. Correct but wasteful: the conclusion
  will be re-derived on the next pass from the survivor. Acceptable churn, or an
  argument for teaching retraction to follow `supersedes` one hop before giving
  up. Not decided.
