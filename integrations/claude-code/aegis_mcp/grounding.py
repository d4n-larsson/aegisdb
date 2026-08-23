"""Grounding: a mention becomes a record id (ROADMAP 5.4 §4).

5.2 made a fact's subject a *record id*, not a bare symbol, so nothing can be
said about "the recall hook" until the recall hook is a record. The convention
is an entity record: a `semantic` record tagged `entity` whose prose names the
thing. Grounding is therefore resolve-or-mint — find that record, or create it.

**The threshold is the whole design, and the two errors are not symmetric.**

Conflation — two things resolved to one id — writes facts about the wrong
entity. Those facts become premises, 5.3 derives further conclusions from them,
and nothing in the system can notice: the triples are well-formed and the
derivations are correct. Fragmentation — one thing split across two ids — only
loses inferences that would have crossed the split. Nothing false is asserted,
and `consolidate` can merge the entity records afterwards, carrying their facts
with them now that a merge preserves assertions.

One is recoverable and the other is not, so a near-miss mints rather than
guesses, and the minting rate is reported: a store that mints for every mention
has a threshold problem that would otherwise show up only as a slowly
fragmenting graph, months later.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

ENTITY_TAG = "entity"

# Identifier-shaped mentions are matched exactly or not at all. `hnsw.c:214` and
# `hnsw.c:215` are two lines of code and one edit apart in similarity, so a
# fuzzy match between them is precisely the conflation this module is built to
# avoid — and unlike prose, an identifier that is "close" is not a paraphrase,
# it is a different thing.
_IDENTIFIER = re.compile(r"[/\\.:_#]|\d")


def looks_like_identifier(mention: str) -> bool:
    return bool(_IDENTIFIER.search(mention)) and len(mention.split()) <= 3


def normalize(mention: str) -> str:
    """Casefold and collapse whitespace — the only normalization applied.

    Deliberately not stemming, not stripping punctuation, not dropping articles.
    Every additional rule is another way for two different things to collide,
    and collision is the expensive error here.
    """
    return " ".join(mention.split()).casefold()


@dataclass
class GroundingResult:
    ids: dict = field(default_factory=dict)  # mention -> record id
    resolved: int = 0
    minted: int = 0
    unresolved: list = field(default_factory=list)  # hit the mint cap

    @property
    def mint_rate(self) -> float:
        total = self.resolved + self.minted
        return self.minted / total if total else 0.0


def _entity_candidates(tools, mention: str, top_k: int, lexical: bool):
    res = tools.search(query=mention, tags=[ENTITY_TAG], kind="semantic",
                       top_k=top_k, lexical=lexical)
    if not res.get("ok"):
        return []
    return [m for m in res.get("memories", []) if m.get("id")]


def resolve(tools, mention: str, config) -> int | None:
    """The id of the entity record this mention denotes, or None.

    Two passes, because one score cannot serve both kinds of mention:

    An **exact** pass first, over lexically-retrieved candidates. This is what
    makes `hnsw.c:214` findable at all — a dense model handles identifiers
    badly — and matching them exactly rather than closely is what keeps two
    adjacent line numbers from becoming one entity.

    Then a **cosine** pass for prose, with a high floor. Note it cannot reuse
    the lexical results: fused scores are on the reciprocal-rank scale, so a
    cosine floor applied to them would either discard everything or admit
    anything, depending on which way the caller guessed. `tools.search`
    documents that trap; this respects it by keeping the two passes separate
    rather than by picking a number for a scale it does not control.
    """
    top_k = int(getattr(config, "grounding_top_k", 5))
    target = normalize(mention)

    for m in _entity_candidates(tools, mention, top_k, lexical=True):
        if normalize(m.get("text", "")) == target:
            return m["id"]

    # An identifier that did not match exactly is a new identifier. Falling
    # through to a similarity score here is how `hnsw.c:214` would come to
    # denote `hnsw.c:215`.
    if looks_like_identifier(mention):
        return None

    floor = float(getattr(config, "grounding_min_score", 0.85))
    for m in _entity_candidates(tools, mention, top_k, lexical=False):
        score = m.get("score")
        if score is not None and score >= floor:
            return m["id"]
    return None


def mint(tools, mention: str) -> int | None:
    """Create the entity record for a mention. Returns its id, or None."""
    res = tools.save(text=mention, tags=[ENTITY_TAG], semantic=True,
                     importance=0.5)
    return res.get("id") if res.get("ok") else None


def ground_mentions(tools, mentions, config) -> GroundingResult:
    """Resolve every mention to an entity id, minting where none is found.

    Minting is capped per call: an extractor facing a long transcript would
    otherwise turn every noun phrase into an entity record. Past the cap a
    mention is reported unresolved rather than guessed at — the caller drops
    the triple, which costs one fact, where a wrong resolution would cost
    every conclusion drawn from it.
    """
    out = GroundingResult()
    budget = int(getattr(config, "grounding_max_mint", 8))
    for mention in mentions:
        if not mention or not mention.strip():
            continue
        if mention in out.ids:
            continue  # same mention twice in one extraction is one entity
        found = resolve(tools, mention, config)
        if found is not None:
            out.ids[mention] = found
            out.resolved += 1
            continue
        if budget <= 0:
            out.unresolved.append(mention)
            continue
        made = mint(tools, mention)
        if made is None:
            out.unresolved.append(mention)
            continue
        out.ids[mention] = made
        out.minted += 1
        budget -= 1
    return out
