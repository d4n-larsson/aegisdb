# Design: Typed Facts & a Predicate Registry (ROADMAP 5.2)

**Status:** Proposed. Second item of Horizon 5, and the first one that touches
the **durable** record format — 5.1 got away with a purely derived index; this
cannot.

**Scope:** Give a record an optional machine-readable form of what it asserts —
a subject/predicate/object triple — alongside the prose that stays in `data`,
make it retrievable by pattern, and declare the predicate vocabulary in config
so later work has something to reason over. **No inference.** Closures,
contradiction detection, and truth maintenance are 5.3; this design's job is to
make them possible without pre-empting them.

## 1. Goals & non-goals

**Goals**

- **A symbol to unify against.** `data` is an opaque blob today, so there is
  nothing for a rule to match on. Without a triple there is no symbolic layer.
- **One entity, one lifecycle.** The triple *is* the record, so TTL, `forget`,
  namespaces, `history`/`as_of`, replication, and compaction all keep working
  with no new cases.
- **A controlled vocabulary.** A declarative registry of predicates, because a
  write path that mints predicates freely (5.4) produces a symbol soup no rule
  can ever fire on. This is the single most load-bearing goal here.
- **Invisible when unused.** A record with no `fact` must behave, and encode,
  exactly as it does today — down to the bytes on disk.

**Non-goals**

- **Not a query language.** `pattern` is one filter field on `search`, in the
  same shape as 4.1's `query`. No variables, no joins, no chaining, no path
  expressions. Variables are the line: the moment a pattern can bind one and
  reuse it, this has become a language and has drifted into the roadmap's
  non-goals.
- **Not inference.** The registry *declares* cardinality, symmetry,
  transitivity, inverse-of and mutex sets in 5.2 and only *acts* on the first
  two of those in 5.3. See §7 for what is enforced now and why.
- **Not typed literals.** An object literal is a string. No numbers (see §5),
  dates, durations, units, or language tags.
- **Not entity resolution.** Deciding that two mentions denote the same thing is
  5.4's job, on top of this.
- **Not a change to edge storage.** An earlier note in
  `symbolic-layer-design.md` predicted 5.2 would force the in-record
  relationship question (`MAX_RELATIONSHIPS`, the per-`relate` record rewrite).
  Re-examined: it does not. A fact is a *field* of a record, not an edge, so it
  adds no adjacency at all. That prediction is withdrawn.

## 2. What exists today

A `MemoryRecord` carries `id`, `type`, timestamps, `importance`, `confidence`,
`agent_id`, `tags[]`, embeddings, `relationships[]`, and an opaque `data` blob
(`record.h`). Retrieval is by time, tag, vector, BM25 text, or graph walk.

So a memory can say *"the recall hook defaults to embedding_mode none"* in prose
and be found by its words — but nothing can ask *"what does the recall hook
default to?"* and get one answer, and nothing can notice a second record
asserting a different default. Tags are a flat set with no subsumption, so
nothing inherits either.

The binary codec is versioned: `record_encode` writes `RECORD_CODEC_VERSION`
(currently 2) as its first byte and `record_decode` accepts 1 and 2, branching
on it (`record.c:438` reads `vec_count` only for v2+). That precedent is what
makes a v3 tractable.

## 3. The triple is the record

`insert` gains an optional `fact` object beside `data`:

```json
{
  "operation": "insert",
  "type": "semantic",
  "data": "The recall hook defaults to embedding_mode=none.",
  "fact": { "s": 42, "p": "defaults_to", "o": "none" }
}
```

- **`s` — subject.** A record id. The entity is a record, so it inherits an
  `agent_id`, and tenant isolation over facts is the isolation already shipped
  rather than a second mechanism to get wrong.
- **`p` — predicate.** A short interned string, validated against the registry.
- **`o` — object.** Either `{"id": N}` (a reference to another record) or a
  literal string. Numbers are deliberately absent — see §5.

`data` keeps the natural-language rendering. It is not derived from the triple
and the triple is not parsed out of it: they are two views the writer supplies,
and the prose stays the thing a human and an LLM read. 5.4's extractor will
produce both.

**Why the subject is an id rather than a bare symbol.** The alternative —
interned entity strings, so `"hnsw.c"` needs no record — avoids minting a record
per entity, which is genuinely tempting for an LLM write path. It was rejected
because it needs a *second* namespace mechanism for tenant isolation (a symbol
table is global; records are not), and because two symbol spaces that can both
name the same thing is precisely the fragmentation 5.4 exists to prevent.

The cost is real: you cannot state a fact about something until it is a record.
The intended remedy is a convention, not a mechanism — an **entity record** is
just a `semantic` record whose `data` is the entity's name, conventionally
tagged `entity`. Minting one is an ordinary `insert`, and 5.4's entity
resolution becomes find-or-create over exactly the dedup machinery 2.2 already
ships. See §13.

## 4. Durable format: codec v3

Three fields appended after `relationships` and before `data`, so the existing
prefix is byte-identical:

```
u8   fact_kind      0 = none, 1 = object is an id-ref, 2 = string
                    (3 reserved for a number; absent in v2, where a record
                     decodes with fact_kind = 0)
u64  fact_subject   record id                      } only when fact_kind != 0
u32+ fact_predicate length-prefixed string         }
     fact_object    u64 | length-prefixed string, per fact_kind
```

Length prefixes are u32 with a `0xFFFFFFFF` NULL marker, matching the codec's
existing `put_lenstr`/`get_lenstr` rather than introducing a second convention.

A decoder that meets a `fact_kind` it does not know **refuses the frame**. It
cannot do otherwise: it does not know how many bytes the unknown object
occupies, so continuing would desync the cursor and decode the payload as
garbage. Adding kind 3 later is therefore a codec bump, not a compatible
extension — which is exactly what the version byte is for.

**Records encode as v2 unless they carry a fact.** This is the whole
compatibility story and it is worth stating plainly: `record_encode` emits
version 3 *only* when `fact_kind != 0`. So

- every existing log stays readable, unchanged;
- a deployment that never uses facts produces byte-identical frames forever, so
  nothing about its on-disk or replication behaviour changes;
- the first v3 frame appears exactly when someone first writes a fact.

`record_decode` accepts 1, 2, and 3, reading the fact fields only for v3.

**Replication needs a version gate, and did not have one.** The handshake
carried `generation` and an auth token but no codec version. A v3-writing primary
streaming to an older replica sends frames whose `record_decode` returns -1 — the
replica cannot apply them and cannot explain why.

The handshake now carries a `codec_version`: the highest record codec the replica
can decode, defaulting to v2 when absent (a build predating the field tops out
there by definition). **Enforcement is per frame, not at subscribe time** — a
deliberate change from this design's first draft, which said the primary should
refuse a replica that "cannot read what it may write". That would have broken
every working mixed-version pair the moment the primary was upgraded, over a
frame the primary may never send: with the v2-unless-fact rule, a cluster that
writes no facts emits no v3 frame at all. So the primary checks each frame's
version byte — which it already has in hand, having read the payload to stream
it — and when one exceeds what the replica declared it sends `MSG_INCOMPATIBLE`
instead, naming the offset and both versions in its log. The replica logs the
same and **stops following**: the offending frame is already durable on the
primary, so reconnecting cannot help and only a new binary can.

Failing on the frame that is actually unreadable, with both versions named, is
better than failing the handshake for a hypothetical one — and far better than
failing frame 400,000 with a bare decode error.

## 5. The fact index

**Three** tables, derived and in-RAM, in the established shape (`tag_index` →
`lexical_index` → `edge_index`). This design first said two, keyed on
`(subject, predicate)` and `(predicate, object)`; implementing §6 showed that
does not cover the filter it exists to serve. A pattern may bind any non-empty
subset of the three positions, and a composite `(subject, predicate)` key cannot
answer `{s}` alone — everything about a subject — without scanning it, nor `{p}`
alone at all. So the predicate moves into the postings instead of the key:

| Table | Answers |
|---|---|
| `subject` → `[(predicate, record)]` | `{s}`, `{s,p}` |
| `object` → `[(predicate, record)]` | `{o}`, `{p,o}` |
| `predicate` → `[record]` | `{p}` |

The first two mirror `edge_index`'s target table exactly — open-addressed,
power-of-two, grown at a 3/4 load factor, tombstones dropped on rehash, postings
sorted so a predicate's run within a slot is contiguous and found by binary
search. The third is a flat array indexed by predicate id: predicates are capped
and few, so it needs no hashing at all.

Predicates are interned, as `edge_index` interns edge kinds and for the same
reasons (integer compares, fixed-width postings, a bounded table against an
untrusted string). **Objects are not**: an object literal is arbitrary caller
text, so a table of them would grow without bound. The object table is keyed by
the literal's hash and compares the stored text on a hit, so a collision costs a
probe rather than a wrong answer.

One deliberate difference from `edge_index`: an un-internable predicate is
**refused**, where an un-internable edge kind was indexed-but-unlabelled. The
reasoning inverts because the consequence does. An unlabelled edge is still
reachable — the edge exists, only its filter precision suffers — whereas a fact
whose predicate cannot be named is unreachable by every pattern that mentions
it. Silently keeping an unqueryable entry is worse than declining it and saying
so.

Never checkpointed; `recovery.c` rebuilds all three from the live set alongside
time/tag/lexical/edge. Keyed on ids and interned predicates, so compaction is a
non-event.

`--no-fact-index` opts out, and a `pattern` search then returns `NOT_READY`,
exactly as `query` does under `--no-lexical-index`.

Object literals need a hashable form for the `(p, o)` side, which is what
settled §13's question about numbers: **there are none.** A float object would
key the index on IEEE-754 bits, where `1` and `1.0` collide but `0.1 + 0.2` and
`0.3` do not — and nothing exact can be asserted about a float in the first
place. Shipping that into a *durable* format for symmetry's sake is the kind of
foot-gun that cannot be withdrawn later, whereas kind 3 stays reserved and can
be added the day a predicate genuinely needs it. Strings hash directly.

## 6. `pattern` on `search`

```json
{ "operation": "search", "pattern": { "s": 42, "p": "defaults_to" }, "top_k": 20 }
```

Any of `s`/`p`/`o` may be omitted or `"*"` to mean "anything". **At least one
must be bound** — an all-wildcard pattern is a full scan wearing a filter's
clothes, and is refused with `INVALID_REQUEST`. Combines with the existing
`type`/`tags`/`agent_id`/time filters by intersection.

A pattern search is a **filter, not a ranking**: like a tags-only search it
orders by time and reports `explain.semantic`/`lexical` as false. Combining
`pattern` with `embedding` or `query` narrows the candidate set and then ranks
it, which is the useful composition and costs no new machinery.

What makes this a filter rather than a language, stated so a future change can
be measured against it: there are no variables, so nothing can be bound in one
position and referred to in another; there is no disjunction; and a pattern
never produces a join. "Facts about the subjects of these facts" is two calls,
deliberately.

## 7. The predicate registry

A JSON file, `--predicate-registry <path>`, loaded at startup:

```json
{
  "defaults_to":  { "object": "literal", "cardinality": "one" },
  "part_of":      { "object": "id", "transitive": true, "inverse_of": "contains" },
  "contains":     { "object": "id", "inverse_of": "part_of" },
  "is_a":         { "object": "id", "transitive": true },
  "conflicts_with": { "object": "id", "symmetric": true },
  "port":         { "object": "string", "cardinality": "one" }
}
```

**Enforced in 5.2:**

- a `fact` whose predicate is absent from the registry is rejected
  (`INVALID_REQUEST`) — this is the controlled vocabulary, and it is the point;
- the object's kind must match the declared `object` (`id` or `string`);
- the file itself is validated at startup: unknown keys, an `inverse_of` naming
  an absent predicate, or an `inverse_of` that is not mutual is a startup
  failure, not a runtime surprise.

**Declared but not yet acted on:** `cardinality`, `transitive`, `symmetric`,
`inverse_of`, `mutex_with`. 5.3 consumes them.

Deliberately: with **no** registry configured, any predicate is accepted. A
server that has not opted into a vocabulary should not be broken by this
feature, and the strictness is worth nothing until someone is writing facts on
purpose.

**Why enforce membership in 5.2 rather than waiting for 5.3.** A registry that
only sits in memory would be untestable and unmotivated, and the failure it
prevents is the one that kills LLM-built knowledge graphs: predicates
proliferating until no two facts share one. Enforcing membership is what makes
the registry a *contract* the 5.4 extractor can be prompted against.

## 8. Locking, recovery, replication, compaction

- **Locking.** The fact indexes live under `db->index_lock` like every other
  in-RAM index; the write path already holds it exclusively at each site. No new
  lock, no change to the `index_lock` → `log_lock` order.
- **Maintenance sites.** The same set 5.1 enumerated: `qe_insert` (a fact
  arrives with the record here, unlike edges), `qe_update` (a fact is
  immutable — see §13), `qe_delete`, `db_replica_apply`, and the `recovery.c`
  rebuild. `qe_relate` is not among them.
- **Recovery** rebuilds both indexes in the pass that already walks the live
  set. Unlike 5.1's edges, no target-liveness check is needed for the
  `(s,p)` side — a fact is a property of the record holding it. An `o` that is a
  dangling id-ref is the same shape of problem the edge index has, and gets the
  same answer: index it, and let the reader find the target absent.
- **Compaction** is unaffected: keys are ids and interned predicates, not log
  offsets.

## 9. Observability

`stats` gains `facts` (records carrying one), `fact_predicates` (distinct
predicates in use — refcounted against live facts, so it survives a restart, the
correction 5.1's `edge_kinds` needed), and `fact_bytes`, which joins the
`index_bytes` total that `--max-index-bytes` enforces. The exporter picks these
up with no edit, since 5.1 made its per-index byte gauge derive from whatever
the server reports.

## 10. Testing

**Unit.** Codec round-trips for all four `fact_kind` values, plus the two
compatibility properties that matter: a record with no fact encodes to **exactly
the bytes v2 produced** (assert against a golden buffer, not just a round-trip),
and a v2 frame decodes to `fact_kind == 0`. Fact-index add/remove/dedup, interning
and its cap, byte accounting, NULL-index tolerance. Registry parsing, including
each rejection case.

**Contract.** A fact written and retrieved by pattern in each of the three bound
positions; an all-wildcard pattern refused; `pattern` intersecting with tags and
with `embedding`; an unregistered predicate refused *and* accepted when no
registry is configured; an object-kind mismatch refused; `NOT_READY` under
`--no-fact-index`; restart parity for both indexes; replica parity; and tenant
isolation — a pattern search must not surface a co-tenant's fact, and the
subject id in a response must not leak the existence of a record the caller
cannot read.

**Eval.** None. 5.2 changes what can be *asked*, not what ranks. The eval
harness enters at 5.3, where inference has recall consequences — the same
division 5.1 drew.

## 11. Rollout (PR sequence)

1. **Codec v3** — the three fields, encode-as-v2-unless-fact, decode for 1/2/3,
   and the golden-bytes compatibility test. No API, no index, no way to set a
   fact yet. Reviewable in isolation, and the riskiest change in the sequence
   because it is the only durable one.
2. **Replication codec gate** — the handshake carries a max supported codec
   version and a primary refuses a replica that cannot read v3. Independent of
   everything above it, and worth landing before facts can be written rather
   than after.
3. **`fact_index.h/.c`** + unit tests, unwired.
4. **Wiring** — `fact` on `insert`, the five maintenance sites, the recovery
   rebuild, `--no-fact-index`, `stats`.
5. **The registry** — parse, validate at startup, enforce membership and object
   kind.
6. **`pattern` on `search`** — the reader, plus the contract tests.

Steps 1 and 2 are worth landing even if the rest slips: a versioned codec with a
version-gated replication handshake is a strictly better position than today's
un-negotiated one, regardless of facts.

## 12. Risks

- **The registry is the whole bet.** If predicates proliferate anyway — because
  the registry is optional and the extractor is permissive — 5.3 has nothing to
  fire on and this horizon produces a slower version of what already exists.
  The mitigation is the in-vocabulary rate 5.4 must measure; it should be a
  number in the eval harness, not an assumption.
- **A durable format change is not reversible.** Once a v3 frame is written, a
  downgrade cannot read that log. §4's v2-unless-fact rule keeps the blast
  radius to deployments that opted in, and the release notes must say so
  plainly.
- **"One more field on `search`" is how a query language starts.** §6 names the
  specific line (variables) rather than gesturing at restraint.

## 13. Open questions

- **Is a fact immutable?** `update` today can change `data`, tags, importance,
  confidence — but a semantic record's *meaning* changing while its triple stays
  put would be worse than either. Leaning: a fact is set at insert and never
  patched; changing what a record asserts means superseding it, which is the
  mechanism 2.1/2.2 already provide and which leaves an auditable chain. That
  also keeps the fact indexes out of the `update` path entirely.
- **Should `o` as an id-ref also create an edge?** It is tempting — the reverse
  edge index would then answer "what facts point at this record?" for free. But
  it doubles the write cost of a fact (a `relate` rewrites the record) and
  couples two features that are cleanly separate. Leaning no, and letting the
  `(p, o)` index answer that question instead.
- **Entity records need a convention, not a mechanism — but which?** §3 proposes
  a `semantic` record tagged `entity` whose `data` is the name. That is enough
  to build on, but nothing enforces it, and 5.4 will be the first real user.
  Worth revisiting once there is a caller instead of a hypothesis.
- ~~**Numeric objects at all?**~~ **Decided (PR 1): no.** Only id-ref and string
  objects exist; value 3 is reserved. A durable format is the wrong place to add
  something for symmetry, because it cannot be taken back out — and a float
  object is a trap on both the index key (IEEE-754 bits) and the semantics
  (nothing exact is assertable). Adding it later costs a codec bump, which is
  cheap next to having shipped it.
