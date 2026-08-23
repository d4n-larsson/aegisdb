# Design: The Neuro-Symbolic Seam (ROADMAP 5.4)

**Status:** Proposed. Fourth and last item of Horizon 5, and the first one where
a model is in the loop at all — 5.1, 5.2 and 5.3 are entirely deterministic.

**Scope:** Make the symbolic layer usable by something that only speaks prose.
Three jobs for the model, all at the **boundary**: turning text into candidate
triples on the way in, turning a question into a `pattern` and a derivation into
English on the way out, and settling a contradiction 5.3 found but cannot
resolve. **The model never participates in inference itself.**

## 1. Goals & non-goals

**Goals**

- **A symbol for prose that was never written as one.** 5.2 gave facts a wire
  format and 5.3 gave them consequences, but both require a caller who already
  thinks in triples. Almost nothing does.
- **The registry as a real constraint.** A model that mints predicates freely
  produces a vocabulary nothing can reason over — the standard way a
  model-built knowledge graph dies. The 5.2 registry exists precisely so the
  extractor has a closed list to be prompted against, and this is the item that
  proves it was worth having.
- **One symbol per thing.** Two phrasings of one entity must resolve to one
  record id, or the store fragments and no rule fires across the split.
- **Traceability.** A wrong answer must land on either a bad premise or a bad
  parse. "The model decided" is not an acceptable resting place for a
  conclusion this system asserted.
- **Off by default, degrading to today.** Per the horizon's ground rules: the
  provider seam already defaults to `none`, and nothing about retrieval changes
  when it stays there.

**Non-goals**

- **The model does not infer.** It does not decide what follows from what, does
  not walk a chain, and does not produce a proof. 5.3 does that, deterministically
  and without a model call, and the whole value of this horizon is that the
  reasoning stays auditable.
- **Not a natural-language query language.** Formulating a `pattern` from a
  question is a convenience over the existing filter, not a new surface. If the
  model cannot express a question as a pattern, the query falls back to
  retrieval — which is what would have happened anyway.
- **Not a general entity-resolution service.** Grounding resolves a mention
  against *this store's* entity records. It does not deduplicate the corpus at
  large; `consolidate` already does that, and now preserves facts while doing it.
- **Not automatic conflict resolution at scale.** Adjudication sees the cases
  5.3 flags, one at a time. A model asked to arbitrate every candidate fact is
  back to being the system of record.

## 2. What exists today

`aegis_mcp/extract.py` already has the seam this hangs on: an
`ExtractionProvider` with `available()` and `extract(text, max_facts)`, backed by
`none` (default), `fake` (deterministic, dependency-free, which is what makes
any of this testable), `claude-code`, `anthropic` and `openai`. It returns a
`Fact` dataclass — `text`, `importance`, `confidence`, `tags` — and it already
carries a second judgment method, `judge_supersedes`, so the shape of "ask the
model a bounded question about specific records" is established rather than new.

It also already treats the transcript as untrusted, framing it as data in the
prompt and telling the model to ignore instructions inside it. That matters more
here than it did for prose (§7).

On the server side: the registry declares the vocabulary and refuses an
undeclared predicate at insert; `pattern` filters on typed facts; `--inference`
materializes closures and reports contradictions it will not settle; and
`explain.derivation` already emits the proof in a machine-readable form — the
model reads that, it does not construct it.

## 3. Parse: prose to candidate triples

Extend the provider with a second target alongside `extract`:

```python
def extract_triples(self, text: str, vocab: list[PredicateSpec],
                    max_triples: int) -> list[CandidateTriple] | None
```

A `CandidateTriple` is deliberately *not* a `Fact`: it names its subject and
object as **strings** — the mentions as they appeared — because the model has no
way to know record ids, and inventing them is the one failure that would be
unrecoverable. Grounding (§4) turns mentions into ids; until then a candidate is
a proposal, not a record.

**Prompted against the registry, and validated against it afterwards.** The
vocabulary goes into the prompt as a closed list with each predicate's object
kind, and every returned triple is then checked against the registry again
before anything is written. Prompting alone is not enforcement: a model told to
pick from a list will still occasionally invent, and the whole point of the
registry is that nothing reaches the log without matching it.

**A rejected triple is dropped and counted, never coerced.** No fuzzy matching
of `is_part_of` onto `part_of`, no "closest declared predicate". Coercion would
turn a measurable failure — the in-vocabulary rate — into a silent corruption of
what the corpus asserts, and the rate is the number this horizon is judged on.

**Nothing is lost by rejection.** A fact is the machine-readable half of a
record; the prose stays in `data` and stays searchable by every path that
existed before. An extraction that yields no triple degrades to exactly what
2.1's extraction does today, which is the ground rule for the whole horizon.

**Extracted facts carry lower confidence than asserted ones** (§7), which
matters more than it looks: 5.3 propagates confidence as a product, so a
conclusion drawn from parsed facts is automatically weaker than one drawn from
facts a human wrote.

## 4. Grounding: one symbol per thing

The hard part, and the one the roadmap names as such.

5.2 made a fact's subject a **record id**, not a bare symbol — so "the recall
hook" has to already be a record before anything can be said about it. The
convention is an entity record: a `semantic` record tagged `entity` whose prose
names the thing. Grounding is therefore: given a mention, find that record, or
create it.

**Resolve with the machinery that already exists.** A mention is searched
against entity records — semantic for paraphrase, lexical for the
identifier-shaped mentions (`hnsw.c:214`) a dense model handles badly. This is
2.2's candidate-and-collapse shape rather than a new mechanism.

**Two passes, not one fused ranking** — a correction from building it. This
section originally said "fused by reciprocal rank", but `tools.search` already
documents why that does not work for a caller with a threshold: fused scores are
on the RRF scale, so a cosine floor applied to them either discards everything
or admits anything depending on which way the caller guessed. So the lexical
pass is used for **exact** matching only, ignoring its score entirely, and the
cosine floor governs a separate semantic pass.

That split turns out to be better than the fusion would have been, for the
reason §4 is about: **an identifier matches exactly or not at all.** `hnsw.c:214`
and `hnsw.c:215` are one character apart and are different things, so a
similarity score between them is not evidence of anything — falling through to
one is exactly how the expensive error happens. Prose gets the high floor;
identifiers get equality.

**The floor is a cosine, and the score is not.** `score_record` blends
similarity as `sim * (0.5 + 0.5 * importance) * confidence`, so an entity
record minted at the default importance of 0.5 can score at most `sim * 0.75`.
Comparing a 0.85 floor against that value directly makes reuse *mathematically
impossible*: every paraphrase mints, the store fragments in exactly the way this
section is written to prevent, and the mint rate sits near 1.0 with no
misconfiguration to point at. Grounding therefore divides the modulation back
out before comparing, which also keeps the floor correct for hand-authored
entity records at any importance rather than only for the ones it minted.

That bug shipped in the first cut of PR 2 and the unit tests could not see it,
because the fake returned raw cosines where the real search returns blended
ones. **A fake kinder than production hides precisely the bug it exists to
catch** — the fake now blends exactly as `score_record` does.

**Prefer fragmentation to conflation.** The threshold is the whole design here,
and the two errors are not symmetric:

- *Conflation* — two things resolved to one id — writes facts about the wrong
  entity. Those facts are then premises, so 5.3 derives further wrong
  conclusions from them, and nothing in the system can detect it: the triples
  are well-formed and the derivations are correct.
- *Fragmentation* — one thing split across two ids — loses inferences that
  would have crossed the split. Nothing false is asserted, and `consolidate`
  can merge the two entity records afterwards, carrying their facts with them
  now that a merge preserves assertions.

One is recoverable and the other is not, so the threshold sits high and a
near-miss mints a new entity rather than guessing. The rate of minting is
reported, because a store that mints an entity for every mention has a
threshold problem that would otherwise only show up as a slowly fragmenting
graph.

**Minting is capped per extraction.** An unbounded extractor facing a long
transcript could otherwise turn every noun phrase into an entity record.

## 5. Verbalize and formulate: the read path

Two directions, both thin.

**Question to `pattern`.** The model is given the registry and asked to express
a question as a pattern — `{"s": <mention>, "p": "defaults_to"}` with the
subject grounded by §4. If it cannot, or the pattern returns nothing, the query
falls back to ordinary retrieval. This is strictly an addition: no query that
works today stops working, which is the `--no-lexical-index` discipline the
ground rules ask for.

**Derivation to English.** `explain.derivation` already carries the rule, the
depth, the premises and whether each is still live. The model renders that as
prose: *"because hnsw.c is part of the storage layer, and the neighbour-selection
loop is part of hnsw.c."*

**The model reads the proof; it never produces it.** This is the line that makes
the horizon worth building. A verbalization is a rendering of a derivation that
already exists and can be checked against the record — not an explanation
generated alongside an answer, which is the arrangement that makes model
"reasoning" unfalsifiable. If the prose and the derivation disagree, the
derivation is right.

## 6. Adjudicate: the inverse of the usual arrangement

5.3 finds contradictions deterministically and refuses to resolve them, because
choosing between two conflicting facts needs to know which is newer, which
source is better, or what the world is like. A model knows some of that.

So: **symbolic detection, neural resolution.** When `conflicts` is non-zero, the
adjudicator is handed *that one pair* — both records' prose, both triples, both
timestamps — and asked which supersedes which, or neither.

Three constraints:

- **It sees only the hard cases.** Not every candidate fact, which is both the
  cheap arrangement and the one where a model error is bounded to a case the
  system had already flagged as unresolvable.
- **A verdict is written as a supersession, never an edit.** `supersedes` is the
  existing mechanism, it leaves an auditable chain, and it keeps facts immutable
  — the model's judgment becomes a record, not a rewrite of history.
- **"Neither" is a first-class answer**, and the default when the model is
  unavailable or unsure. An unresolved conflict stays reported, which is exactly
  what 5.3 does today; adjudication is an improvement on that state, not a
  requirement for it.

## 7. The transcript is untrusted

`extract.py` already frames the transcript as data. Triples raise the stakes:
prose is read by a human who can discount it, while a fact is a premise that
5.3 will draw further conclusions from without asking anyone.

Four things bound it, none of them new:

- **The registry.** A poisoned transcript cannot invent a predicate; it can only
  use ones the operator declared, which is a far smaller surface than free text.
- **Namespaces.** An extracted fact inherits the writing agent's namespace, and
  5.3's job never joins premises across namespaces, so a poisoned transcript
  cannot reach another tenant's conclusions.
- **Confidence.** Extracted facts are written below the default, and 5.3
  multiplies along a chain, so conclusions resting on them rank below
  conclusions resting on asserted facts.
- **Provenance.** Every derived record names its premises, so a bad conclusion
  is traceable to the parse that caused it. That is the horizon's "done when",
  and it is also the incident-response story.

What none of that bounds is a *plausible* false fact in a declared predicate.
That is a real residual risk and it is stated rather than mitigated: the
defence is that extraction is opt-in, its output is confidence-marked, and its
provenance is inspectable.

## 8. Config & observability

| Flag / setting | Default | Effect |
|---|---|---|
| `extraction_triples` | off | propose triples alongside prose facts |
| `triple_max_per_extraction` | 16 | bound on candidates per transcript |
| `grounding_min_score` | high | below this a mention mints a new entity |
| `grounding_max_mint` | 8 | new entity records per extraction |
| `adjudicate_conflicts` | off | hand flagged contradictions to the model |

`stats` gains what the metric needs: `triples_proposed`, `triples_accepted`
(the two that give the **in-vocabulary rate**), `entities_resolved`,
`entities_minted`, and `conflicts_adjudicated`.

The in-vocabulary rate is the headline number and belongs in the dashboard next
to recall, because it is the one that says whether the registry is working as a
contract or merely as a filter that throws away most of what the model proposes.

## 9. Testing

The `fake` backend is what makes this testable at all, and it needs a triple
target: deterministic, dependency-free, and — importantly — able to emit
**out-of-vocabulary** predicates on demand, so the rejection path is exercised
rather than assumed.

Contract tests:

1. A transcript yields triples that appear as facts, findable by `pattern`.
2. An out-of-vocabulary predicate is rejected, counted, and the prose is still
   captured — degradation, not failure.
3. Two phrasings of one entity ground to the same id; two different entities do
   not.
4. A mention below threshold mints exactly one entity record, capped.
5. Extraction inherits the writing namespace, and a co-tenant cannot see it.
6. With the provider `none`, nothing about capture changes — byte-identical
   behaviour to today.
7. A conflict handed to the adjudicator produces a `supersedes`, not an edit,
   and "neither" leaves the conflict reported.
8. A derivation verbalizes without the model being able to alter it: the
   rendered prose changes, the `explain.derivation` payload does not.

**`make eval` gains an extraction dataset**: transcripts paired with the triples
a careful reader would write. The gate is the **in-vocabulary rate** — measured,
per the roadmap, not asserted — plus a grounding accuracy that counts conflation
and fragmentation separately, since §4 argues they are not equally bad and a
single accuracy number would hide that.

This is also where the multi-hop eval's honest caveat gets addressed. That
dataset is synthetic and built to have the property under test; an extraction
dataset drawn from real transcripts is the first evidence that questions of that
shape occur in text nobody wrote for the purpose.

## 10. Rollout (PR sequence)

1. **`CandidateTriple` + the `fake` triple backend**, with registry validation
   and the accept/reject counters. No model, no grounding, no writes — the
   rejection path and the in-vocabulary metric exist before anything can
   produce a triple.
2. **Grounding** — resolve-or-mint against entity records, with the threshold,
   the mint cap, and both counters. Testable with `fake` alone.
3. **Wire it to capture** — extraction proposes, grounding resolves, the server
   validates, facts land. Contract tests 1, 2, 5, 6.
4. **The real providers** — `claude-code`, `anthropic`, `openai` triple targets.
   Nothing structural; the prompt and the parser.
5. **Read path** — question to `pattern`, derivation to English. Contract tests
   8.
6. **Adjudication.** Contract test 7.
7. **The extraction eval dataset** and its gates.

PRs 1–3 are the spine: after 3 the seam works end to end with a deterministic
backend, which is the version whose behaviour can be reasoned about. 4 makes it
useful; 5 and 6 are independently revertible.

## 11. Risks

- **The registry is too small to be useful.** If a careful reader's triples are
  mostly out of vocabulary, the honest response is a bigger registry, not
  coercion — but a registry that grows to fit every transcript stops being a
  contract. Where that line sits is an empirical question the eval answers.
- **Grounding drifts as the corpus grows.** A threshold tuned on a small store
  may conflate on a large one, where more entities means more near-misses. The
  minting rate is the leading indicator, which is why it is reported.
- **Adjudication is a model in the write path**, however narrowly. It is off by
  default and writes only supersessions, but it is the one place where a model
  error becomes durable state.
- **Verbalization will be trusted more than it deserves.** A fluent rendering of
  a derivation reads as authoritative. The payload it renders is right there in
  the response, which is the only real defence.

## 12. Open questions

- **Should grounding reuse `consolidate`'s threshold?** Both are "are these two
  records the same thing?", and having two tunables for one judgment invites
  them to drift apart. Against: consolidation merges *memories* while grounding
  matches *entities*, and the asymmetry argued in §4 does not apply to
  consolidation, which can be symmetric about its errors.
- **Where do entity records come from before extraction runs?** The first
  transcript on an empty store mints everything, which is correct but means the
  earliest facts are the ones most likely to be fragmented — they had nothing to
  resolve against. Possibly an argument for a seeding pass, possibly an argument
  for re-grounding old facts after the entity set stabilizes.
- **Does the object position need grounding too?** An `id`-valued object is a
  record reference and clearly does. A `string`-valued one is a literal — but
  "five seconds" and "5s" are the same literal to a reader and different ones to
  the index. 5.2 deliberately has no literal normalization; this is where the
  cost of that shows up.
- **Should a rejected triple be surfaced to the writer?** The in-vocabulary rate
  makes rejection measurable in aggregate, but an agent whose fact was dropped
  currently has no way to know. Telling it invites retry loops; not telling it
  makes the vocabulary invisible to the thing most affected by it.
