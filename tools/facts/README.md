# A starter vocabulary, and two corpora that use it

Typed facts (ROADMAP 5.2) need a **predicate registry** — the vocabulary a fact
may be written in. Nothing else in this repo shipped one, so anyone turning the
feature on had to invent a vocabulary before they could see it work. These three
files are that starting point:

| | |
|---|---|
| [`predicates.example.json`](../../predicates.example.json) | ten predicates for coding-agent memory, at the repo root beside `.env.example` |
| [`aegisdb.json`](aegisdb.json) | 20 entities and 37 facts about what *this project* is made of |
| [`aegisdb-surface.json`](aegisdb-surface.json) | 90 entities and 169 facts about what it *exposes* — operations, retrieval modes, flags, parameters |

## Run it

```bash
./build/aegisdb --data-dir ./data --port 9470 \
    --predicate-registry predicates.example.json \
    --inference --inference-interval-sec 5 &

make seed-facts          # or: python3 tools/facts/seed.py --port 9470
make seed-surface        # the second corpus; either order, or on its own
```

Both targets take `SEED_ARGS`, which is passed to `seed.py` verbatim —
`SEED_ARGS='--dry-run'`, `SEED_ARGS='--port 9470 --namespace mine'`.

`--dry-run` reports what would be written without writing it. Re-running is
safe: an entity whose exact prose already exists is reused and a fact the
corpus already asserts is skipped, because the point is to run it, look at the
result, adjust the corpus, and run it again.

In Docker, mount the registry and name it in `AEGIS_EXTRA_ARGS` — see the
commented lines in `docker-compose.yml`.

## What the vocabulary is for

Ten predicates, and deliberately not more. A registry that grows to fit every
sentence stops being a contract, which is the failure mode ROADMAP 5.2 exists
to prevent — so this covers what a coding agent actually needs to say and
stops:

- `is_a`, `part_of` / `contains`, `depends_on` — structure. `part_of` and
  `contains` are declared `inverse_of` each other, so asserting one gives you
  the other.
- `defaults_to`, `guarded_by`, `measured_by`, `owned_by` — the properties a
  developer keeps re-asking about: what does this default to, which flag turns
  it off, how do I know it works, whose is it.
- `deprecated_by` / `recommended_by` — declared `mutex_with` each other, so a
  record saying both is reported as a contradiction rather than silently held.

`defaults_to` and `owned_by` are `cardinality: one`: a second live value is a
contradiction the inference job flags. That is the point of declaring them —
the constraint does the work a model would otherwise be asked to do on every
write.

## What `aegisdb.json` demonstrates

37 asserted facts become **65**, with 28 derived. Two of the queries that get
interesting:

**"Which flag disables an in-RAM derived index?"** No record says that. It
needs `is_a` membership joined to `guarded_by`:

```
keyword search    -> definitions, not answers
pattern + subsume -> --no-lexical-index, --no-edge-index,
                     --no-fact-index, --no-usage-feedback
```

The last one arrives through two hops — `usage_index.c is_a a checkpointed
index is_a an in-RAM derived index` — and no single record states it.

**`contains` is never asserted.** All 19 of them are derived from `part_of` by
the `inverse_of` rule.

## What `aegisdb-surface.json` adds

The first corpus describes the internals — the layers, the index files, what
depends on what. This one describes the surface a client actually meets: the 20
operations, the 6 retrieval modes, the 19 flags and 13 request parameters, the 3
memory types, and for each of them what it defaults to or which flag turns it
off. Kept separate rather than merged, because they answer different questions
and either is worth loading alone.

On its own: 169 asserted facts become **236**, with 67 derived.

Its own useful shape is the **class entity** — `a wire operation`, `a retrieval
mode`, `a server flag`, `a request parameter` — asserted with `is_a` and then
used as a bound `pattern.s` under `subsume`. That turns a category into a query:

```
{"s": <a retrieval mode>, "p": "guarded_by", "subsume": true}
  -> --no-fact-index      (pattern search)
     --no-lexical-index   (a query search)
     --no-edge-index      (direction:in / direction:both)
     --inference          (subsume itself)
```

which is "what do I lose by turning an index off?", answered without any record
naming a retrieval mode as a group. The same shape over `defaults_to` lists all
19 flag defaults, or all 13 parameter defaults, in one lookup.

**The two compose.** Twelve entities are written with the same prose in both
files (`the query layer`, `hnsw.c`, `the append-only log`, `the inference job`,
…), so `seed.py`'s exact-prose reuse binds them to the same records rather than
minting a second set. Loading both, 206 asserted facts become **367**, with 161
derived — well past the 95 the two produce apart, because the chains now
join: this corpus says each operation is `part_of` the query layer, the other
says the query layer is `part_of` the server, and transitivity walks the rest.
43 things end up `part_of` the server, and all 87 `contains` facts are still
derived rather than written.

## A caveat worth keeping

Both corpora are hand-written, so they say what a careful reader believes rather
than what the code does. They will drift from the repo as the repo changes;
nothing checks them. That is fine for a worked example and would not be fine for
a fact base anyone relied on — which is itself the honest lesson about
hand-maintained knowledge, and the reason ROADMAP 5.4 puts a model at the
write path instead.

`aegisdb.json` also earned its keep once already: loading it is what exposed
[#269](https://github.com/d4n-larsson/aegisdb/issues/269), where every derived
conclusion read as `#4 part_of #1` and no keyword query could reach it. The
multi-hop eval never caught that, because it scores by record *label* rather
than by what the record says.
