"""Unit tests for LLM fact extraction (ROADMAP 2.1)."""
import os
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.extract import (Fact, FakeExtractionProvider, NoneExtractionProvider,
                               _parse_facts, make_extraction_provider)


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