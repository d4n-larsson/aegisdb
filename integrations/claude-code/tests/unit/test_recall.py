"""Unit tests for recall context formatting: per-memory truncation + budget."""
import os
import sys
import time
import unittest
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.embeddings import FakeProvider
from aegis_mcp.recall import format_context, run_recall
from aegis_mcp.tools import MemoryTools, _suppress_near_duplicates


def _mems(*texts):
    return [{"id": i + 1, "text": t, "tags": []} for i, t in enumerate(texts)]


def _cfg(budget_ms):
    return SimpleNamespace(
        recall_time_budget_ms=budget_ms, recall_top_k=3, recall_min_score=0.0,
        recall_dedup_threshold=0.95, recall_max_chars_per_memory=0,
        recall_char_budget=0, embedding_dimensions=16, namespace="t")


class _InstantClient:
    """A fake AegisClient that replies immediately (no socket)."""
    def __init__(self, resp=None):
        self._resp = resp if resp is not None else {"ok": True, "records": []}

    def request(self, payload, read_timeout_ms=None):
        return self._resp


class TestDedup(unittest.TestCase):
    def test_suppress_near_duplicates_keeps_top_and_distinct(self):
        a = (0.9, {"id": 1, "embedding": [1.0, 0.0, 0.0]})
        b = (0.8, {"id": 2, "embedding": [1.0, 0.0, 0.0]})  # identical to a
        c = (0.7, {"id": 3, "embedding": [0.0, 1.0, 0.0]})  # distinct
        kept = _suppress_near_duplicates([a, b, c], 0.95)
        ids = [r["id"] for _, r in kept]
        self.assertEqual(ids, [1, 3])  # highest-scored dup + the distinct one

    def test_threshold_out_of_range_disables(self):
        a = (0.9, {"id": 1, "embedding": [1.0, 0.0]})
        b = (0.8, {"id": 2, "embedding": [1.0, 0.0]})
        self.assertEqual(len(_suppress_near_duplicates([a, b], 1.0)), 2)
        self.assertEqual(len(_suppress_near_duplicates([a, b], 0.0)), 2)

    def test_records_without_embeddings_are_kept(self):
        a = (0.9, {"id": 1})  # no embedding
        b = (0.8, {"id": 2})
        self.assertEqual(len(_suppress_near_duplicates([a, b], 0.95)), 2)

    def test_search_drops_near_duplicate_memories(self):
        # Two records with identical embeddings + one distinct; semantic search
        # should return the top dup and the distinct one, not both dups.
        fp = FakeProvider(16)
        same = fp.embed_document("the deploy command is make ship")
        diff = fp.embed_document("the mascot is an otter")
        recs = [
            {"id": 1, "type": "semantic", "data": "deploy with make ship",
             "importance": 0.9, "confidence": 1.0, "embedding": same},
            {"id": 2, "type": "semantic", "data": "to deploy run make ship",
             "importance": 0.5, "confidence": 1.0, "embedding": same},
            {"id": 3, "type": "semantic", "data": "otter mascot",
             "importance": 0.5, "confidence": 1.0, "embedding": diff},
        ]
        cfg = SimpleNamespace(
            recall_top_k=10, recall_min_score=0.0, recall_dedup_threshold=0.95,
            embedding_mode="fake", embedding_dimensions=16, namespace="t")
        tools = MemoryTools(cfg, _InstantClient({"ok": True, "records": recs}), fp)
        res = tools.search(query="how do I deploy")
        ids = sorted(m["id"] for m in res["memories"])
        self.assertEqual(ids, [1, 3])  # id 2 (near-dup of 1) suppressed


class TestRunRecallBudget(unittest.TestCase):
    def test_slow_embedder_is_bounded_by_budget(self):
        # embed_query runs before the socket; a slow/cold embedder must not be
        # able to stall the turn past the recall budget.
        class SlowProvider(FakeProvider):
            def embed_query(self, text):
                time.sleep(2.0)  # far exceeds the 200ms budget
                return super().embed_query(text)

        start = time.monotonic()
        result = run_recall("deploy?", _cfg(200), SlowProvider(16),
                            client=_InstantClient())
        elapsed = time.monotonic() - start

        self.assertTrue(result.degraded)
        self.assertEqual(result.memories, [])
        # Bounded by the 200ms budget, NOT the 2s embed call.
        self.assertLess(elapsed, 1.0)

    def test_fast_path_returns_result_not_degraded(self):
        # When the work finishes within budget, the worker's result propagates.
        result = run_recall("hello", _cfg(1000), FakeProvider(16),
                            client=_InstantClient({"ok": True, "records": []}))
        self.assertFalse(result.degraded)
        self.assertEqual(result.memories, [])


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