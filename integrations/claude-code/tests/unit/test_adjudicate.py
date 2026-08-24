"""The verdict parser and the abstention rule (ROADMAP 5.4 §6).

Every test here is about the same thing from a different angle: a verdict
tombstones a record, so anything short of an unambiguous answer must read as
abstention. Being lenient about what counts as "no" is safe; being lenient
about what counts as "yes" deletes somebody's fact.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from aegis_mcp.adjudicate import adjudicate_conflicts  # noqa: E402
from aegis_mcp.extract import (ADJUDICATE_A, ADJUDICATE_B,  # noqa: E402
                               ADJUDICATE_NEITHER, ExtractionProvider,
                               FakeExtractionProvider, _parse_verdict)


class _Cfg:
    adjudicate_conflicts = True
    adjudicate_max_per_run = 8


class _Tools:
    """The three calls the adjudicator makes, and a record of what it did."""

    def __init__(self, pairs, records, ok=True):
        self.pairs = pairs
        self.records = records
        self.ok = ok
        self.related = []
        self.deleted = []

    def conflicts(self, limit=None):
        if not self.ok:
            return {"ok": False, "error": "invalid"}
        return {"ok": True, "conflicts": self.pairs[:limit],
                "total": len(self.pairs)}

    def get(self, id):
        rec = self.records.get(id)
        return {"ok": True, "memory": rec} if rec else {"ok": False}

    def relate(self, from_id, to_id, kind=None):
        self.related.append((from_id, to_id, kind))
        return {"ok": True}

    def delete(self, id):
        self.deleted.append(id)
        self.records.pop(id, None)
        return {"ok": True}


def _rec(i, text):
    return {"id": i, "text": text, "fact": {"s": 1, "p": "defaults_to",
                                            "o": text}, "updated": 1000 + i}


class TestParseVerdict(unittest.TestCase):
    def test_the_three_answers(self):
        self.assertEqual(_parse_verdict("A"), ADJUDICATE_A)
        self.assertEqual(_parse_verdict(" b \n"), ADJUDICATE_B)
        self.assertEqual(_parse_verdict("NEITHER"), ADJUDICATE_NEITHER)
        self.assertEqual(_parse_verdict("neither, they differ"),
                         ADJUDICATE_NEITHER)

    def test_anything_unclear_abstains(self):
        for raw in ("", None, "I think A supersedes B", "A or B",
                    "It depends.", "```", "{\"verdict\": \"A\"}", "AB",
                    "Answer: A"):
            self.assertEqual(_parse_verdict(raw), ADJUDICATE_NEITHER,
                             f"{raw!r} must not read as a decision")

    def test_a_prose_reply_naming_both_is_not_a_choice(self):
        """The failure this rule exists for. "A supersedes B" and "B supersedes
        A" are opposite verdicts and share a first letter, so a parser reading
        the first token it recognised would get one of them exactly backwards
        — and delete the fact the model meant to keep."""
        self.assertEqual(_parse_verdict("A supersedes B"), ADJUDICATE_NEITHER)
        self.assertEqual(_parse_verdict("B supersedes A"), ADJUDICATE_NEITHER)


class TestProviderDefault(unittest.TestCase):
    def test_the_base_provider_abstains(self):
        """A backend that has not implemented adjudication must not decide."""
        self.assertEqual(ExtractionProvider().adjudicate({}, {}),
                         ADJUDICATE_NEITHER)

    def test_the_fake_needs_exactly_one_marked_side(self):
        stale = FakeExtractionProvider.STALE_MARKER
        f = FakeExtractionProvider()
        self.assertEqual(f.adjudicate({"text": f"x {stale}"}, {"text": "y"}),
                         ADJUDICATE_B)
        self.assertEqual(f.adjudicate({"text": "x"}, {"text": f"y {stale}"}),
                         ADJUDICATE_A)
        self.assertEqual(f.adjudicate({"text": "x"}, {"text": "y"}),
                         ADJUDICATE_NEITHER)
        self.assertEqual(
            f.adjudicate({"text": f"x {stale}"}, {"text": f"y {stale}"}),
            ADJUDICATE_NEITHER, "both marked is not a decision")


class TestAdjudicateLoop(unittest.TestCase):
    def test_a_verdict_links_before_it_deletes(self):
        """Order matters: `relate` against a tombstone is refused, so deleting
        first and failing to link would leave a removed record with nothing
        naming what replaced it."""
        tools = _Tools([{"a": 2, "b": 3}],
                       {2: _rec(2, "none"), 3: _rec(3, "local")})

        class Always(ExtractionProvider):
            def adjudicate(self, a, b):
                return ADJUDICATE_A

        res = adjudicate_conflicts(tools, _Cfg(), Always())
        self.assertEqual(res.resolved, 1)
        self.assertEqual(tools.related, [(2, 3, "supersedes")])
        self.assertEqual(tools.deleted, [3])

    def test_an_exception_is_not_a_decision(self):
        tools = _Tools([{"a": 2, "b": 3}],
                       {2: _rec(2, "none"), 3: _rec(3, "local")})

        class Angry(ExtractionProvider):
            def adjudicate(self, a, b):
                raise RuntimeError("backend down")

        res = adjudicate_conflicts(tools, _Cfg(), Angry())
        self.assertEqual((res.resolved, res.abstained), (0, 1))
        self.assertEqual(tools.deleted, [], "nothing is deleted on an error")

    def test_a_side_that_has_gone_is_skipped_not_guessed(self):
        tools = _Tools([{"a": 2, "b": 3}], {2: _rec(2, "none")})

        class Always(ExtractionProvider):
            def adjudicate(self, a, b):
                raise AssertionError("must not be asked about a missing side")

        res = adjudicate_conflicts(tools, _Cfg(), Always())
        self.assertEqual((res.skipped, res.considered), (1, 0))

    def test_a_record_with_no_triple_left_is_not_adjudicated(self):
        """The claim of this path is that the *symbols* found the conflict. A
        record whose fact is gone is no longer the thing that was flagged, and
        reasoning about its prose alone would quietly become the arrangement
        2.1 shipped."""
        prose_only = {"id": 3, "text": "local", "updated": 1003}
        tools = _Tools([{"a": 2, "b": 3}], {2: _rec(2, "none"), 3: prose_only})

        class Always(ExtractionProvider):
            def adjudicate(self, a, b):
                raise AssertionError("must not be asked")

        res = adjudicate_conflicts(tools, _Cfg(), Always())
        self.assertEqual((res.skipped, res.considered), (1, 0))

    def test_an_older_server_is_nothing_to_adjudicate(self):
        tools = _Tools([], {}, ok=False)
        res = adjudicate_conflicts(tools, _Cfg(), FakeExtractionProvider())
        self.assertEqual((res.seen, res.considered, res.resolved), (0, 0, 0))

    def test_the_cap_bounds_a_bad_run(self):
        cfg = _Cfg()
        cfg.adjudicate_max_per_run = 2
        pairs = [{"a": i, "b": i + 1} for i in (2, 4, 6, 8)]
        tools = _Tools(pairs, {i: _rec(i, str(i)) for i in range(2, 10)})
        res = adjudicate_conflicts(tools, cfg, FakeExtractionProvider())
        self.assertEqual(res.seen, 2, "the server is asked for no more")

    def test_a_cap_of_zero_asks_nothing(self):
        cfg = _Cfg()
        cfg.adjudicate_max_per_run = 0
        tools = _Tools([{"a": 2, "b": 3}],
                       {2: _rec(2, "none"), 3: _rec(3, "local")})
        res = adjudicate_conflicts(tools, cfg, FakeExtractionProvider())
        self.assertEqual((res.seen, res.considered), (0, 0))


if __name__ == "__main__":
    unittest.main()
