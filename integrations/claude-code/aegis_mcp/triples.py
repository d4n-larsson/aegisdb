"""Wiring the seam: extraction proposes, grounding resolves, the server judges.

ROADMAP 5.4 PR 3. The three pieces exist separately — `extract.extract_triples`
proposes candidates, `extract.validate_triples` checks them against the
registry, `grounding.ground_mentions` turns mentions into record ids — and this
is the only place they meet.

Three properties the arrangement is built to keep:

- **The server has the last word.** Client-side validation against the same
  registry is an optimization: it avoids a round-trip for a triple that would be
  refused. `insert` enforces the vocabulary regardless, and a write that fails
  there is counted, not retried differently.
- **Nothing is lost by rejection.** A dropped triple costs the machine-readable
  half of one record. The prose facts extraction produced are stored by the
  ordinary path either way, so a transcript that yields no usable triple behaves
  exactly as it does with this feature off.
- **Never raises.** Capture documents that a backend failure means zero stored,
  not an exception, and a triple is strictly an addition to that path.
"""
from __future__ import annotations

from dataclasses import dataclass

from .extract import validate_triples
from .grounding import ground_mentions


@dataclass
class TripleStoreResult:
    stored: int = 0
    proposed: int = 0
    rejected: int = 0  # out of vocabulary, client-side
    ungrounded: int = 0  # no entity id, and the mint cap was spent
    duplicate: int = 0  # the corpus already asserts exactly this
    failed: int = 0  # the server refused the write
    entities_resolved: int = 0
    entities_minted: int = 0

    @property
    def in_vocabulary_rate(self) -> float:
        """The number 5.4 is judged on. Reported even when nothing was stored,
        because a registry that rejects most of what the model proposes and one
        that fits the corpus look identical from the accepted count alone."""
        return (self.proposed - self.rejected) / self.proposed \
            if self.proposed else 0.0


def render(subject: str, predicate: str, obj: str) -> str:
    """Prose for a triple-bearing record.

    `insert` refuses an empty payload, and a record that turns up in a search
    result should be readable by whoever finds it. Underscores become spaces so
    `part_of` reads as English; nothing cleverer, because a template that tried
    to conjugate would be wrong more often than the flat form is ugly.
    """
    return f"{subject} {predicate.replace('_', ' ')} {obj}".strip()


def _mentions_of(triples, spec_of):
    """Every mention needing an id: the subject always, the object when its
    predicate is declared id-valued. A string-valued object is a literal and
    must not be grounded — turning "none" into an entity record would both
    invent a thing and lose the value."""
    out = []
    for t in triples:
        out.append(t.subject)
        spec = spec_of.get(t.predicate)
        if spec is not None and spec.object == "id":
            out.append(t.obj)
    return out


def _already_asserted(tools, fact) -> bool:
    """Does the corpus already hold exactly this triple?

    An exact `pattern` lookup, which is what 5.2's fact index is for: all three
    positions bound, so it is an index probe rather than a scan, and cheap
    enough to ask once per write. A failure to answer is read as "not present"
    — writing a duplicate is a smaller error than dropping a fact the corpus
    does not have.
    """
    try:
        res = tools.search(pattern=fact, top_k=1)
    except Exception:
        return False
    return bool(res.get("ok") and res.get("memories"))


def store_triples(tools, text, vocab, config, extractor) -> TripleStoreResult:
    """Propose, validate, ground and write. Returns what happened at each step.

    `vocab` is None when no registry is configured, in which case this does
    nothing at all: the vocabulary is the contract, and proposing triples with
    nothing to check them against is not a smaller version of the feature.
    """
    res = TripleStoreResult()
    if not getattr(config, "extract_triples", False) or vocab is None:
        return res

    try:
        cands = extractor.extract_triples(
            text, vocab, int(getattr(config, "extract_max_triples", 16)))
    except Exception:
        return res  # never let a triple break capture
    if not cands:
        return res

    checked = validate_triples(cands, vocab)
    res.proposed = checked.proposed
    res.rejected = len(checked.rejected)
    if not checked.accepted:
        return res

    spec_of = {p.name: p for p in vocab}
    try:
        grounded = ground_mentions(tools, _mentions_of(checked.accepted,
                                                       spec_of), config)
    except Exception:
        return res
    res.entities_resolved = grounded.resolved
    res.entities_minted = grounded.minted

    # Clamped: the server validates confidence into [0, 1] and refuses the
    # whole insert outside it, which would surface as every triple `failed`
    # with nothing naming the setting responsible.
    confidence = min(1.0, max(0.0,
                              float(getattr(config,
                                            "extract_triple_confidence", 0.6))))
    for t in checked.accepted:
        subject_id = grounded.ids.get(t.subject)
        if subject_id is None:
            res.ungrounded += 1
            continue
        spec = spec_of.get(t.predicate)
        fact = {"s": subject_id, "p": t.predicate}
        if spec is not None and spec.object == "id":
            object_id = grounded.ids.get(t.obj)
            if object_id is None:
                res.ungrounded += 1
                continue
            fact["o"] = {"id": object_id}
        else:
            fact["o"] = t.obj
        if _already_asserted(tools, fact):
            # A stable convention gets restated every session, and the prose
            # path already guards against that with supersession. Without the
            # same guard here one identical assertion accumulates per capture,
            # and 5.3's derivation walk then traverses every copy.
            res.duplicate += 1
            continue
        try:
            out = tools.save(render(t.subject, t.predicate, t.obj),
                             tags=["fact"], semantic=True,
                             importance=0.5, confidence=confidence,
                             fact=fact)
        except Exception:
            res.failed += 1
            continue
        if out.get("ok"):
            res.stored += 1
        else:
            # The server enforces the vocabulary too, and its answer is the one
            # that counts. A refusal here means the client-side copy of the
            # registry has drifted from the server's — worth counting, not
            # worth retrying differently.
            res.failed += 1
    return res
