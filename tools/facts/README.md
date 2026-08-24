# A starter vocabulary, and a corpus that uses it

Typed facts (ROADMAP 5.2) need a **predicate registry** — the vocabulary a fact
may be written in. Nothing else in this repo shipped one, so anyone turning the
feature on had to invent a vocabulary before they could see it work. These two
files are that starting point:

| | |
|---|---|
| [`predicates.example.json`](../../predicates.example.json) | ten predicates for coding-agent memory, at the repo root beside `.env.example` |
| [`aegisdb.json`](aegisdb.json) | 20 entities and 37 facts about *this project*, written in that vocabulary |

## Run it

```bash
./build/aegisdb --data-dir ./data --port 9470 \
    --predicate-registry predicates.example.json \
    --inference --inference-interval-sec 5 &

make seed-facts          # or: python3 tools/facts/seed.py --port 9470
```

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

## What it demonstrates

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

## A caveat worth keeping

The corpus is hand-written, so it says what a careful reader believes rather
than what the code does. It will drift from the repo as the repo changes;
nothing checks it. That is fine for a worked example and would not be fine for
a fact base anyone relied on — which is itself the honest lesson about
hand-maintained knowledge, and the reason ROADMAP 5.4 puts a model at the
write path instead.

This corpus also earned its keep once already: loading it is what exposed
[#269](https://github.com/d4n-larsson/aegisdb/issues/269), where every derived
conclusion read as `#4 part_of #1` and no keyword query could reach it. The
multi-hop eval never caught that, because it scores by record *label* rather
than by what the record says.
