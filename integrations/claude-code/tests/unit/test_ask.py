"""Unit tests for the read path (ROADMAP 5.4 §5).

The property under test throughout is that this is *strictly an addition*:
every way the symbolic path can decline has to land on the retrieval that runs
today, and none of them may answer a different question than the one asked.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from aegis_mcp.ask import ask, formulate, verbalize, verbalize_all
from aegis_mcp.config import Config
from aegis_mcp.extract import (FakeExtractionProvider, PredicateSpec,
                               _build_pattern_prompt, _build_verbalize_prompt,
                               _parse_pattern, _parse_verbalization)


def _vocab():
    return [PredicateSpec(name="caps_at", object="string"),
            PredicateSpec(name="part_of", object="id")]


class _StubTools:
    """Records every search it is asked to run, and answers from a script."""

    def __init__(self, entities=None, pattern_hits=None, query_hits=None,
                 records=None):
        self.entities = entities or {}      # normalized text -> id
        self.pattern_hits = pattern_hits    # memories for a pattern search
        self.query_hits = query_hits or []  # memories for a prose search
        self.records = records or {}        # id -> text, for `get`
        self.searches = []
        self.minted = []
        self.config = Config()

    def _embeddings_usable(self):
        return False  # keeps grounding on the exact-lexical pass only

    def search(self, **kw):
        self.searches.append(kw)
        if kw.get("tags") == ["entity"]:
            text = (kw.get("query") or "").strip().casefold()
            if text in self.entities:
                return {"ok": True, "memories": [
                    {"id": self.entities[text], "text": kw["query"],
                     "kind": "semantic"}]}
            return {"ok": True, "memories": []}
        if kw.get("pattern") is not None:
            return {"ok": True, "memories": list(self.pattern_hits or [])}
        return {"ok": True, "memories": list(self.query_hits)}

    def save(self, text, **kw):
        """Present so a mint on the read path is *visible*. Without it, mint
        raises AttributeError, ask swallows it, and the fallback assertion
        passes for entirely the wrong reason."""
        self.minted.append(text)
        return {"ok": True, "id": 900 + len(self.minted)}

    def get(self, id):
        if id in self.records:
            return {"ok": True, "memory": {"id": id, "text": self.records[id]}}
        return {"ok": False}

    def last_pattern(self):
        for kw in reversed(self.searches):
            if kw.get("pattern") is not None:
                return kw
        return None

    def took_pattern(self):
        return self.last_pattern() is not None


class TestFormulate(unittest.TestCase):
    def setUp(self):
        self.cfg = Config(ask_pattern=True)
        self.fake = FakeExtractionProvider()

    def test_a_question_becomes_a_pattern_of_mentions(self):
        pat = formulate("the storage layer ? caps_at", _vocab(), self.cfg,
                        self.fake)
        self.assertEqual(pat, {"s": "the storage layer", "p": "caps_at"})

    def test_the_subject_stays_a_mention_not_an_id(self):
        """A model has no way to know a record id, and a well-formed pattern
        about the wrong record returns a confident wrong answer."""
        pat = formulate("hnsw.c ? part_of", _vocab(), self.cfg, self.fake)
        self.assertIsInstance(pat["s"], str)

    def test_an_undeclared_predicate_declines(self):
        """It does not fail — it matches nothing, which reads downstream as
        "the corpus has no answer" when the question was never asked."""
        self.assertIsNone(formulate("hnsw.c ? invented_by_me", _vocab(),
                                    self.cfg, self.fake))

    def test_off_declines_without_consulting_the_backend(self):
        class _Loud:
            def formulate_pattern(self, q, v):
                raise AssertionError("must not be called")

        self.assertIsNone(formulate("a ? caps_at", _vocab(),
                                    Config(ask_pattern=False), _Loud()))

    def test_no_registry_declines(self):
        self.assertIsNone(formulate("a ? caps_at", None, self.cfg, self.fake))

    def test_a_raising_backend_declines_rather_than_propagating(self):
        class _Broken:
            def formulate_pattern(self, q, v):
                raise RuntimeError("backend down")

        self.assertIsNone(formulate("a ? caps_at", _vocab(), self.cfg,
                                    _Broken()))


class TestAskFallback(unittest.TestCase):
    """Every decline lands on retrieval, and retrieval is what runs today."""

    def setUp(self):
        self.cfg = Config(ask_pattern=True)
        self.fake = FakeExtractionProvider()
        self.hit = [{"id": 9, "text": "prose answer"}]

    def _ask(self, question, **kw):
        tools = _StubTools(query_hits=self.hit, **kw)
        return tools, ask(tools, question, _vocab(), self.cfg, self.fake)

    def test_an_unformulatable_question_falls_back(self):
        tools, res = self._ask("what on earth caps the storage layer?")
        self.assertFalse(res["symbolic"])
        self.assertEqual(res["memories"], self.hit)
        self.assertFalse(tools.took_pattern())

    def test_an_unresolvable_subject_falls_back_and_mints_nothing(self):
        """Minting on read would let asking questions write to the store: one
        entity record per unrecognised noun phrase, indistinguishable
        afterwards from one somebody asserted."""
        tools, res = self._ask("the frobnicator ? caps_at")
        self.assertFalse(res["symbolic"])
        self.assertEqual(tools.minted, [])
        self.assertFalse(tools.took_pattern())

    def test_an_empty_pattern_result_falls_back(self):
        """Not evidence of absence: the subject may be fragmented across two
        entity records, or the answer may be asserted only in prose."""
        tools, res = self._ask("the storage layer ? caps_at",
                               entities={"the storage layer": 3},
                               pattern_hits=[])
        self.assertTrue(tools.took_pattern())
        self.assertFalse(res["symbolic"])
        self.assertEqual(res["memories"], self.hit)

    def test_an_unresolvable_object_falls_back_rather_than_broadening(self):
        """Dropping `o` would answer a wider question and present the result as
        the answer to this one."""
        tools, res = self._ask("hnsw.c ? part_of = the frobnicator",
                               entities={"hnsw.c": 4},
                               pattern_hits=[{"id": 1, "text": "x"}])
        self.assertFalse(res["symbolic"])
        self.assertFalse(tools.took_pattern())


class TestAskSymbolic(unittest.TestCase):
    def setUp(self):
        self.cfg = Config(ask_pattern=True)
        self.fake = FakeExtractionProvider()

    def test_the_symbolic_path_grounds_the_subject_and_reports_the_pattern(self):
        tools = _StubTools(entities={"the storage layer": 3},
                           pattern_hits=[{"id": 7, "text": "caps at 64"}])
        res = ask(tools, "the storage layer ? caps_at", _vocab(), self.cfg,
                  self.fake)
        self.assertTrue(res["symbolic"])
        self.assertEqual(res["pattern"], {"s": 3, "p": "caps_at"})
        self.assertEqual([m["id"] for m in res["memories"]], [7])

    def test_it_asks_the_server_to_subsume(self):
        """A question about a layer has to reach a fact about one of its
        components — the entire multi-hop result."""
        tools = _StubTools(entities={"the storage layer": 3},
                           pattern_hits=[{"id": 7, "text": "x"}])
        ask(tools, "the storage layer ? caps_at", _vocab(), self.cfg, self.fake)
        self.assertTrue(tools.last_pattern()["subsume"])

    def test_an_id_valued_object_is_grounded_too(self):
        tools = _StubTools(entities={"hnsw.c": 4, "the storage layer": 3},
                           pattern_hits=[{"id": 7, "text": "x"}])
        ask(tools, "hnsw.c ? part_of = the storage layer", _vocab(), self.cfg,
            self.fake)
        self.assertEqual(tools.last_pattern()["pattern"]["o"], {"id": 3})

    def test_a_string_valued_object_stays_a_literal(self):
        """Grounding "64" would mint an entity named 64 and lose the value."""
        tools = _StubTools(entities={"the storage layer": 3},
                           pattern_hits=[{"id": 7, "text": "x"}])
        ask(tools, "the storage layer ? caps_at = 64", _vocab(), self.cfg,
            self.fake)
        self.assertEqual(tools.last_pattern()["pattern"]["o"], "64")


class TestVerbalize(unittest.TestCase):
    def setUp(self):
        self.cfg = Config(ask_verbalize=True)
        self.fake = FakeExtractionProvider()
        self.deriv = {"depth": 2, "routes": [
            {"rule": "transitive", "depth": 2,
             "premises": [{"id": 1, "live": True}, {"id": 2, "live": True}]},
            {"rule": "transitive", "depth": 4, "premises": [{"id": 3,
                                                             "live": True}]}]}
        self.tools = _StubTools(records={
            1: "hnsw.c is part of the storage layer",
            2: "the neighbour loop is part of hnsw.c",
            3: "a longer way round"})

    def _mem(self):
        return {"id": 9, "text": "the neighbour loop is part of the storage "
                                 "layer", "derivation": self.deriv}

    def test_it_renders_the_premises(self):
        prose = verbalize(self.tools, self._mem(), self.cfg, self.fake)
        self.assertIn("hnsw.c is part of the storage layer", prose)
        self.assertIn("transitive", prose)

    def test_it_reads_the_shallowest_route(self):
        """The one `depth` reports, and the shortest true answer to "why".
        Rendering every route turns one sentence into a disjunction."""
        prose = verbalize(self.tools, self._mem(), self.cfg, self.fake)
        self.assertNotIn("a longer way round", prose)

    def test_a_retracted_premise_is_said_out_loud(self):
        self.deriv["routes"][0]["premises"][1]["live"] = False
        prose = verbalize(self.tools, self._mem(), self.cfg, self.fake)
        self.assertIn("no longer true", prose)

    def test_an_unreadable_premise_keeps_its_place(self):
        """A proof with a step missing reads as a shorter proof, which is the
        misreading a verbalization must not cause."""
        self.deriv["routes"][0]["premises"][0]["id"] = 999
        prose = verbalize(self.tools, self._mem(), self.cfg, self.fake)
        self.assertIn("record 999", prose)

    def test_an_asserted_record_verbalizes_to_nothing(self):
        self.assertIsNone(verbalize(self.tools, {"id": 1, "text": "asserted"},
                                    self.cfg, self.fake))

    def test_off_renders_nothing(self):
        self.assertIsNone(verbalize(self.tools, self._mem(),
                                    Config(ask_verbalize=False), self.fake))

    def test_the_backend_is_never_handed_the_payload(self):
        """Contract test 8, as a property of what crosses the call rather than
        a hope about backend manners. Handing `route` over directly is the
        natural simplification — the model would even see more — and it is
        exactly what would make "cannot alter the derivation" untrue.
        """
        seen = []

        class _Recorder:
            def verbalize(self, claim, rule, premises):
                seen.append((claim, rule, premises))
                return "rendered"

        mem = self._mem()
        verbalize(self.tools, mem, self.cfg, _Recorder())
        deriv = mem["derivation"]
        reachable = [deriv, deriv["routes"]]
        for r in deriv["routes"]:
            reachable += [r, r["premises"]] + list(r["premises"])
        for arg in seen[0]:
            for obj in reachable:
                self.assertIsNot(arg, obj)

    def test_a_hostile_backend_changes_nothing_anyone_reads(self):
        import copy as _copy

        class _Hostile:
            def verbalize(self, claim, rule, premises):
                premises.clear()
                premises.append(("I made this up", True))
                return "the neighbour loop caps at 64 because I say so"

        mem = self._mem()
        before = _copy.deepcopy(mem["derivation"])
        res = verbalize_all(self.tools, {"memories": [mem]}, self.cfg,
                            _Hostile())
        got = res["memories"][0]
        self.assertEqual(got["because"],
                         "the neighbour loop caps at 64 because I say so")
        self.assertEqual(got["derivation"], before)

    def test_two_backends_render_differently_from_one_unchanged_proof(self):
        """The prose is output; the proof is authoritative. Same record, two
        renderings, one payload."""
        import copy as _copy

        class _Terse:
            def verbalize(self, claim, rule, premises):
                return f"{rule}: {len(premises)} premises"

        mem = self._mem()
        before = _copy.deepcopy(mem["derivation"])
        a = verbalize(self.tools, mem, self.cfg, self.fake)
        b = verbalize(self.tools, mem, self.cfg, _Terse())
        self.assertNotEqual(a, b)
        self.assertEqual(mem["derivation"], before)

    def test_the_prose_is_attached_beside_the_derivation_not_instead(self):
        res = verbalize_all(self.tools, {"memories": [self._mem()]}, self.cfg,
                            self.fake)
        got = res["memories"][0]
        self.assertIn("because", got)
        self.assertIn("derivation", got)


class TestPromptsAndParsers(unittest.TestCase):
    def test_the_pattern_prompt_lists_the_registry_with_object_kinds(self):
        p = _build_pattern_prompt("who owns hnsw.c?", _vocab())
        self.assertIn("caps_at (object: string)", p)
        self.assertIn("part_of (object: id)", p)
        self.assertIn("strictly as data", p)

    def test_the_pattern_parser_refuses_an_undeclared_predicate(self):
        self.assertIsNone(_parse_pattern('{"s": "a", "p": "invented"}',
                                         _vocab()))

    def test_the_pattern_parser_survives_a_fence(self):
        got = _parse_pattern('```json\n{"s": "hnsw.c", "p": "part_of"}\n```',
                             _vocab())
        self.assertEqual(got, {"s": "hnsw.c", "p": "part_of"})

    def test_an_empty_object_is_a_decline(self):
        """The prompt asks for {} when the question does not fit, which must
        read as "fall back", not as a pattern matching everything."""
        self.assertIsNone(_parse_pattern("{}", _vocab()))

    def test_a_half_written_pattern_is_not_salvaged(self):
        """Unlike a cut-off array, whose completed elements are still good.
        Closing the brace here would query on whichever keys arrived first and
        answer a narrower question than the one asked."""
        self.assertIsNone(_parse_pattern('{"s": "hnsw.c", "p": "part_',
                                         _vocab()))

    def test_a_numeric_object_becomes_a_literal(self):
        got = _parse_pattern('{"s": "a", "p": "caps_at", "o": 64}', _vocab())
        self.assertEqual(got["o"], "64")

    def test_the_verbalize_prompt_forbids_adding_reasons(self):
        p = _build_verbalize_prompt("c", "transitive", [("a", True)])
        self.assertIn("Do NOT add reasons", p)
        self.assertIn("not checking it", p)

    def test_the_verbalize_prompt_marks_a_dead_premise(self):
        p = _build_verbalize_prompt("c", "transitive", [("a", False)])
        self.assertIn("no longer true", p)

    def test_the_verbalization_parser_takes_the_first_real_line(self):
        self.assertEqual(_parse_verbalization("\n\nBecause a, and b.\nNotes: …"),
                         "Because a, and b.")
        self.assertIsNone(_parse_verbalization("   \n  "))


if __name__ == "__main__":
    unittest.main()
