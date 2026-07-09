"""Unit tests for recall context formatting: per-memory truncation + budget."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.recall import format_context


def _mems(*texts):
    return [{"id": i + 1, "text": t, "tags": []} for i, t in enumerate(texts)]


class TestFormatContext(unittest.TestCase):
    def test_empty(self):
        ctx, n = format_context([])
        self.assertEqual(ctx, "")
        self.assertEqual(n, 0)

    def test_unlimited_by_default(self):
        # 0/0 caps => every memory rendered in full (back-compat behavior)
        ctx, n = format_context(_mems("alpha", "beta"))
        self.assertEqual(n, 2)
        self.assertIn("alpha", ctx)
        self.assertIn("beta", ctx)
        self.assertTrue(ctx.startswith("Relevant memories from past sessions:"))

    def test_per_memory_truncation(self):
        long = "x" * 1000
        ctx, n = format_context(_mems(long), max_chars_per_memory=100)
        self.assertEqual(n, 1)
        self.assertIn("[…]", ctx)  # elision is marked
        # the rendered body is bounded (cap + marker + line prefix), not 1000
        self.assertLess(len(ctx), 200)

    def test_word_boundary_truncation(self):
        # truncation backs up to a space rather than slicing mid-word
        ctx, _ = format_context(_mems("alpha beta gamma delta epsilon"),
                                max_chars_per_memory=14)
        self.assertNotIn("gamm ", ctx)   # would be a mid-word cut of "gamma"
        self.assertIn("alpha beta", ctx)
        self.assertIn("[…]", ctx)

    def test_char_budget_drops_tail_and_marks_omission(self):
        # three ~60-char memories, budget fits about one
        mems = _mems("a" * 60, "b" * 60, "c" * 60)
        ctx, n = format_context(mems, char_budget=80)
        self.assertEqual(n, 1)                 # only the top-ranked kept
        self.assertIn("aaaa", ctx)
        self.assertNotIn("bbbb", ctx)
        # the omission is explicit, not silent
        self.assertIn("2 more", ctx)
        self.assertIn("omitted", ctx)

    def test_no_omission_marker_when_all_fit(self):
        ctx, n = format_context(_mems("short one", "short two"))
        self.assertEqual(n, 2)
        self.assertNotIn("omitted", ctx)

    def test_budget_is_a_hard_ceiling_even_for_top_memory(self):
        # per-memory cap OFF but a budget set: the forced top memory must still
        # be bounded by the budget (the gap this fix closes).
        ctx, n = format_context(_mems("z" * 100000), max_chars_per_memory=0,
                                char_budget=200)
        self.assertEqual(n, 1)
        # block stays near the budget (+ small header/prefix/marker overhead),
        # nowhere near the 100k-char memory
        self.assertLess(len(ctx), 400)

    def test_ranked_order_preserved(self):
        ctx, n = format_context(_mems("first", "second", "third"))
        self.assertLess(ctx.index("first"), ctx.index("second"))
        self.assertLess(ctx.index("second"), ctx.index("third"))


if __name__ == "__main__":
    unittest.main()