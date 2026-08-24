"""The vocabulary as it reaches the model (ROADMAP 5.2 / issue #271).

A tool description is prompt text on every request, so what goes in it is a
budget decision as much as a correctness one. These pin both: that a server
without typed facts pays nothing, and that one with them says something a model
can act on.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from aegis_mcp.extract import PredicateSpec  # noqa: E402
from aegis_mcp.server import VOCAB_HINT_MAX, vocabulary_hint  # noqa: E402


def specs(*names):
    return [PredicateSpec(name=n, object="id") for n in names]


class TestVocabularyHint(unittest.TestCase):
    def test_no_vocabulary_adds_nothing_at_all(self):
        """Byte-for-byte the description it has always been. A server without
        typed facts must not pay for the feature in prompt tokens."""
        self.assertEqual(vocabulary_hint(None), "")
        self.assertEqual(vocabulary_hint([]), "")

    def test_the_predicates_are_named(self):
        hint = vocabulary_hint(specs("part_of", "defaults_to"))
        self.assertIn("part_of", hint)
        self.assertIn("defaults_to", hint)

    def test_named_in_a_stable_order(self):
        """Two servers with the same vocabulary produce the same description,
        whatever order the registry happened to enumerate in — otherwise a
        prompt cache is invalidated by nothing at all."""
        a = vocabulary_hint(specs("zeta", "alpha", "mid"))
        b = vocabulary_hint(specs("mid", "zeta", "alpha"))
        self.assertEqual(a, b)
        self.assertLess(a.index("alpha"), a.index("mid"))

    def test_a_large_registry_is_summarised_not_dumped(self):
        """A description is sent on every request. A registry of hundreds would
        otherwise become the largest thing in the context."""
        hint = vocabulary_hint(specs(*[f"p{i:03d}" for i in range(200)]))
        named = sum(1 for i in range(200) if f"p{i:03d}" in hint)
        self.assertEqual(named, VOCAB_HINT_MAX)
        self.assertIn(f"and {200 - VOCAB_HINT_MAX} more", hint)

    def test_it_says_what_to_do_with_them(self):
        """A list of bare names is not actionable. The point is to steer how a
        question is phrased, so the hint has to say that."""
        hint = vocabulary_hint(specs("defaults_to"))
        self.assertIn("falls back", hint,
                      "it must say the non-matching case still works")
        self.assertIn("?", hint, "and give an example question")


if __name__ == "__main__":
    unittest.main()
