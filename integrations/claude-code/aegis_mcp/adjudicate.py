"""Adjudication: symbolic detection, neural resolution (ROADMAP 5.4 §6).

The inverse of the arrangement 2.1 shipped. There, a model judged *every*
candidate fact against its neighbours — an LLM call per fact doing work a
single-valued-predicate constraint does deterministically, at write time, for
free. Here the rules find the contradiction and the model sees only the one
pair they could not settle, which is both the cheaper arrangement and the one
where a model error is bounded to a case the system had already flagged as
unresolvable.

Four properties, and the first three are what make the fourth affordable:

- **A verdict is a supersession, never an edit.** `supersedes` is the existing
  mechanism, it leaves an auditable chain, and it keeps facts immutable — the
  model's judgment becomes a record, not a rewrite of history. The same
  `relate` + `delete` pair capture already uses for prose.
- **"Neither" is first-class and is the default.** Unavailable backend,
  unparseable reply, unsure model, missing record: all of them abstain. An
  unresolved conflict stays reported, which is exactly what 5.3 does today —
  adjudication is an improvement on that state, not a requirement for it.
- **It sees only what the rules flagged.** The pair list comes from the server,
  which built it from the same loop that counts the gauge. Nothing here goes
  looking for contradictions.
- **It is a model in the write path, however narrowly.** That is the honest
  cost, and it is why this is off by default, capped per run, and writes
  nothing but supersessions.
"""
from __future__ import annotations

from dataclasses import dataclass

from .extract import ADJUDICATE_A, ADJUDICATE_B, ADJUDICATE_NEITHER


@dataclass
class AdjudicationResult:
    seen: int = 0  # pairs the server reported
    considered: int = 0  # pairs actually put to the model
    resolved: int = 0  # verdicts written as a supersession
    abstained: int = 0  # "neither" — left reported, which is the safe state
    skipped: int = 0  # a side was already gone by the time we looked
    failed: int = 0  # the verdict could not be written


def _side(tools, rec_id):
    """One side of a pair, as the prompt wants it — or None if it is gone.

    A pair is a snapshot of the last inference pass, and anything may have
    happened since: the record could have been consolidated away, forgotten, or
    tombstoned by an earlier verdict in this very run. Reading both sides fresh
    is what keeps a stale pair from costing a model call and a wrong write.
    """
    got = tools.get(rec_id)
    if not got.get("ok"):
        return None
    mem = got.get("memory") or {}
    if not mem.get("fact"):
        # No triple left means this is no longer the contradiction that was
        # flagged. Abstain rather than reason about the prose alone: the whole
        # claim of this path is that the symbols found the conflict.
        return None
    return {"id": rec_id, "text": mem.get("text"), "fact": mem.get("fact"),
            "when": mem.get("updated") or mem.get("created")}


def adjudicate_conflicts(tools, config, extractor) -> AdjudicationResult:
    """Put each flagged contradiction to the model and write what it decides.

    Never raises. This runs at the end of a session capture, and a backend
    that times out must cost nothing more than an unresolved contradiction —
    the state the corpus was already in.
    """
    res = AdjudicationResult()
    if not getattr(config, "adjudicate_conflicts", False):
        return res

    cap = max(0, int(getattr(config, "adjudicate_max_per_run", 8)))
    if cap == 0:
        return res
    try:
        listed = tools.conflicts(limit=cap)
    except Exception:
        return res
    if not listed.get("ok"):
        # Includes an older server with no such operation. A server that cannot
        # list contradictions does not have any to hand over.
        return res
    pairs = listed.get("conflicts") or []
    res.seen = len(pairs)

    # Ids this run has already tombstoned. The server's list is a snapshot, and
    # a record can appear in more than one pair — a subject with three values
    # for a single-valued predicate produces a star of them. Without this, the
    # second pair naming an already-deleted record would spend a model call to
    # learn what `_side` is about to report anyway.
    gone = set()
    for pair in pairs:
        a_id, b_id = pair.get("a"), pair.get("b")
        if not a_id or not b_id or a_id in gone or b_id in gone:
            res.skipped += 1
            continue
        a = _side(tools, a_id)
        b = _side(tools, b_id)
        if a is None or b is None:
            res.skipped += 1
            continue
        res.considered += 1
        try:
            verdict = extractor.adjudicate(a, b)
        except Exception:
            # An exception is not a decision. Same rule as every other
            # unavailable backend here: abstain.
            res.abstained += 1
            continue
        if verdict == ADJUDICATE_A:
            winner, loser = a_id, b_id
        elif verdict == ADJUDICATE_B:
            winner, loser = b_id, a_id
        else:
            res.abstained += 1
            continue

        # Link first, then tombstone. The other order loses the provenance if
        # the second call fails: `relate` against a tombstone is refused, so a
        # crash between the two would leave a deleted record with nothing
        # naming what replaced it — a silent loss, which is the failure the
        # `supersedes` chain exists to prevent. This way the worst case is a
        # link to a record that is still live, which the next pass re-flags.
        linked = tools.relate(winner, loser, "supersedes")
        if not linked.get("ok"):
            res.failed += 1
            continue
        if not tools.delete(loser).get("ok"):
            res.failed += 1
            continue
        gone.add(loser)
        res.resolved += 1
    return res
