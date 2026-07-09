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
        self.assertIn("…", ctx)
        # the rendered body is bounded (cap + ellipsis + line prefix), not 1000
        self.assertLess(len(ctx), 200)

    def test_char_budget_drops_tail(self):
        # three ~60-char memories, budget fits about one
        mems = _mems("a" * 60, "b" * 60, "c" * 60)
        ctx, n = format_context(mems, char_budget=80)
        self.assertEqual(n, 1)                 # only the top-ranked kept
        self.assertIn("aaaa", ctx)
        self.assertNotIn("bbbb", ctx)

    def test_top_memory_always_included_even_over_budget(self):
        # a single memory larger than the budget is still included (capped)
        ctx, n = format_context(_mems("z" * 500), max_chars_per_memory=50,
                                char_budget=10)
        self.assertEqual(n, 1)
        self.assertIn("zzz", ctx)

    def test_ranked_order_preserved(self):
        ctx, n = format_context(_mems("first", "second", "third"))
        self.assertLess(ctx.index("first"), ctx.index("second"))
        self.assertLess(ctx.index("second"), ctx.index("third"))


if __name__ == "__main__":
    unittest.main()