"""Unit tests for LLM fact extraction (ROADMAP 2.1)."""
import os
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.extract import (CandidateTriple, Fact, FakeExtractionProvider,
                               NoneExtractionProvider, PredicateSpec,
                               _parse_facts, _parse_indices, load_vocabulary,
                               make_extraction_provider, validate_triples)


class TestParseFacts(unittest.TestCase):
    def test_plain_array(self):
        raw = '[{"fact": "deploys via make ship", "importance": 0.9, "tags": ["deploy"]}]'
        facts = _parse_facts(raw, 12)
        self.assertEqual(len(facts), 1)
        self.assertEqual(facts[0].text, "deploys via make ship")
        self.assertEqual(facts[0].importance, 0.9)
        self.assertEqual(facts[0].tags, ["deploy"])

    def test_fenced_json(self):
        raw = 'Here you go:\n```json\n[{"fact": "use tabs"}]\n```\n'
        facts = _parse_facts(raw, 12)
        self.assertEqual(len(facts), 1)
        self.assertEqual(facts[0].text, "use tabs")
        self.assertEqual(facts[0].importance, 0.5)  # default

    def test_prose_wrapped_array(self):
        raw = 'Sure. [{"fact": "x"}, {"text": "y"}] done'
        facts = _parse_facts(raw, 12)
        self.assertEqual([f.text for f in facts], ["x", "y"])

    def test_malformed_returns_empty(self):
        self.assertEqual(_parse_facts("not json at all", 12), [])
        self.assertEqual(_parse_facts("", 12), [])
        self.assertEqual(_parse_facts("{}", 12), [])  # object, not array

    def test_importance_clamped_and_tags_capped(self):
        # out-of-range importance + more than the tag cap
        raw = '[{"fact": "a", "importance": 5, "tags": ["1","2","3","4","5","6","7","8","9","10"]}]'
        f = _parse_facts(raw, 12)[0]
        self.assertEqual(f.importance, 1.0)      # clamped to [0,1]
        self.assertLessEqual(len(f.tags), 8)     # tag cap

    def test_max_facts_limit(self):
        raw = "[" + ",".join('{"fact": "f%d"}' % i for i in range(20)) + "]"
        self.assertEqual(len(_parse_facts(raw, 5)), 5)

    def test_skips_empty_and_nondict(self):
        raw = '["a string", {"fact": ""}, {"fact": "keep"}]'
        self.assertEqual([f.text for f in _parse_facts(raw, 12)], ["keep"])


class TestFakeProvider(unittest.TestCase):
    def test_extracts_substantive_lines(self):
        p = FakeExtractionProvider()
        self.assertTrue(p.available())
        facts = p.extract("We decided to deploy via make ship\nok\nRoot cause was a stale cache", 12)
        self.assertEqual(len(facts), 2)  # 'ok' is too short
        self.assertTrue(all(isinstance(f, Fact) for f in facts))

    def test_dedup_and_cap(self):
        p = FakeExtractionProvider()
        text = "\n".join(["same fact stated here"] * 3 + ["another distinct fact line"])
        facts = p.extract(text, 12)
        self.assertEqual(len(facts), 2)  # deduped
        self.assertEqual(len(p.extract("a b c d\ne f g h\ni j k l", 2)), 2)  # cap


class TestParseIndices(unittest.TestCase):
    def test_valid_and_bounded(self):
        self.assertEqual(_parse_indices("[0, 2]", 3), [0, 2])
        self.assertEqual(_parse_indices("[5]", 3), [])          # out of range dropped
        self.assertEqual(_parse_indices("[1,1,1]", 3), [1])     # deduped
        self.assertEqual(_parse_indices("```json\n[0]\n```", 3), [0])
        self.assertEqual(_parse_indices("supersedes: [1] only", 3), [1])

    def test_malformed_returns_empty(self):
        self.assertEqual(_parse_indices("nope", 3), [])
        self.assertEqual(_parse_indices("", 3), [])
        self.assertEqual(_parse_indices('{"a":1}', 3), [])


class TestJudgeSupersedes(unittest.TestCase):
    def test_none_supersedes_nothing(self):
        self.assertEqual(NoneExtractionProvider().judge_supersedes("x", ["y"]), [])

    def test_fake_supersedes_same_subject_not_identical(self):
        p = FakeExtractionProvider()
        new = "The deploy command is make release"
        cands = [
            "The deploy command is make ship",   # same subject, updated -> supersede
            "The deploy command is make release",  # identical -> duplicate, not supersede
            "Database migrations run with make migrate",  # unrelated
        ]
        self.assertEqual(p.judge_supersedes(new, cands), [0])

    def test_fake_empty(self):
        self.assertEqual(FakeExtractionProvider().judge_supersedes("", ["a b c"]), [])
        self.assertEqual(FakeExtractionProvider().judge_supersedes("a b", []), [])


class TestFactory(unittest.TestCase):
    def test_none_is_unavailable(self):
        p = make_extraction_provider(SimpleNamespace(extract_mode="none"))
        self.assertIsInstance(p, NoneExtractionProvider)
        self.assertFalse(p.available())

    def test_fake_selected(self):
        p = make_extraction_provider(SimpleNamespace(extract_mode="fake"))
        self.assertIsInstance(p, FakeExtractionProvider)
        self.assertTrue(p.available())

    def test_unknown_falls_back_to_none(self):
        p = make_extraction_provider(SimpleNamespace(extract_mode="bogus"))
        self.assertIsInstance(p, NoneExtractionProvider)


if __name__ == "__main__":
    unittest.main()

class TestTripleVocabulary(unittest.TestCase):
    """The registry as a contract (ROADMAP 5.4 §3)."""

    def _vocab(self):
        return [PredicateSpec(name="part_of", object="id"),
                PredicateSpec(name="defaults_to", object="string")]

    def test_declared_predicates_are_kept(self):
        cands = [CandidateTriple("hnsw.c", "part_of", "the storage layer"),
                 CandidateTriple("the hook", "defaults_to", "none")]
        res = validate_triples(cands, self._vocab())
        self.assertEqual(len(res.accepted), 2)
        self.assertEqual(res.proposed, 2)
        self.assertEqual(res.in_vocabulary_rate, 1.0)

    def test_undeclared_predicate_is_rejected_not_coerced(self):
        """`is_part_of` must NOT be mapped onto the declared `part_of`.

        Coercion would turn the in-vocabulary rate — the number 5.4 is judged
        on — into a silent change to what the corpus asserts."""
        cands = [CandidateTriple("hnsw.c", "is_part_of", "the storage layer"),
                 CandidateTriple("the hook", "defaults_to", "none")]
        res = validate_triples(cands, self._vocab())
        self.assertEqual([c.predicate for c in res.accepted], ["defaults_to"])
        self.assertEqual(res.rejected, [("is_part_of", "undeclared")])
        self.assertEqual(res.in_vocabulary_rate, 0.5)

    def test_id_predicate_needs_something_to_ground(self):
        cands = [CandidateTriple("hnsw.c", "part_of", "   ")]
        res = validate_triples(cands, self._vocab())
        self.assertEqual(res.accepted, [])
        self.assertEqual(res.rejected, [("part_of", "empty id object")])

    def test_no_registry_accepts_everything(self):
        """The server accepts any predicate with no registry configured, so
        being stricter here would reject writes that would have succeeded."""
        cands = [CandidateTriple("a", "anything_at_all", "b")]
        res = validate_triples(cands, [])
        self.assertEqual(len(res.accepted), 1)

    def test_rate_is_zero_for_nothing_proposed(self):
        self.assertEqual(validate_triples([], self._vocab()).in_vocabulary_rate,
                         0.0)


class TestLoadVocabulary(unittest.TestCase):
    def test_reads_the_servers_registry_file(self):
        import json as _json
        import tempfile
        with tempfile.NamedTemporaryFile("w", suffix=".json",
                                         delete=False) as fh:
            _json.dump({"part_of": {"object": "id", "transitive": True},
                        "defaults_to": {"object": "string"}}, fh)
            path = fh.name
        vocab = load_vocabulary(path)
        os.unlink(path)
        self.assertEqual(sorted(p.name for p in vocab),
                         ["defaults_to", "part_of"])
        self.assertEqual({p.name: p.object for p in vocab}["part_of"], "id")

    def test_missing_or_unset_path_is_empty_not_an_error(self):
        self.assertEqual(load_vocabulary(""), [])
        self.assertEqual(load_vocabulary("/nonexistent/registry.json"), [])

    def test_malformed_entries_are_skipped(self):
        import json as _json
        import tempfile
        with tempfile.NamedTemporaryFile("w", suffix=".json",
                                         delete=False) as fh:
            _json.dump({"good": {"object": "id"},
                        "no_object": {"transitive": True},
                        "bad_object": {"object": "float"}}, fh)
            path = fh.name
        vocab = load_vocabulary(path)
        os.unlink(path)
        self.assertEqual([p.name for p in vocab], ["good"])


class TestFakeTriples(unittest.TestCase):
    def test_parses_explicit_triple_lines(self):
        p = FakeExtractionProvider()
        out = p.extract_triples("hnsw.c : part_of : the storage layer\n"
                                "noise line with no colons\n", [], 16)
        self.assertEqual(len(out), 1)
        self.assertEqual((out[0].subject, out[0].predicate, out[0].obj),
                         ("hnsw.c", "part_of", "the storage layer"))

    def test_can_emit_out_of_vocabulary_predicates(self):
        """The rejection path is half of what 5.4 is judged on; a fake that
        could only produce valid triples would leave it untested."""
        p = FakeExtractionProvider()
        out = p.extract_triples("a : invented_by_the_model : b", [], 16)
        res = validate_triples(out, [PredicateSpec("part_of", "id")])
        self.assertEqual(res.accepted, [])
        self.assertEqual(res.rejected, [("invented_by_the_model", "undeclared")])

    def test_respects_the_cap(self):
        p = FakeExtractionProvider()
        text = "\n".join(f"s{i} : part_of : o{i}" for i in range(30))
        self.assertEqual(len(p.extract_triples(text, [], 5)), 5)

    def test_other_providers_propose_nothing_by_default(self):
        self.assertEqual(NoneExtractionProvider().extract_triples("x", [], 5),
                         [])
