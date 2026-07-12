"""Unit tests for summary providers + candidate clustering."""
import os
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.summary import (FakeSummaryProvider, NoneSummaryProvider,
                               make_summary_provider)
from aegis_mcp.summarize import _clusters


def _cfg(**kw):
    base = dict(summary_mode="fake", summary_model="", summary_max_importance=0.6,
                summary_min_cluster=2, summary_max_cluster=20,
                summary_max_clusters_per_run=20)
    base.update(kw)
    return SimpleNamespace(**base)


def _mem(i, tags, kind="episodic", importance=0.5, text="t"):
    return {"id": i, "kind": kind, "tags": tags, "importance": importance,
            "text": text}


class TestProviders(unittest.TestCase):
    def test_none_disabled_by_default(self):
        self.assertFalse(make_summary_provider(SimpleNamespace(summary_mode="none")).available())
        self.assertIsInstance(make_summary_provider(SimpleNamespace(summary_mode="none")),
                              NoneSummaryProvider)

    def test_fake_summarizes(self):
        p = FakeSummaryProvider()
        self.assertTrue(p.available())
        out = p.summarize(["the deploy uses make ship", "staging step is flaky"])
        self.assertIsNotNone(out)
        text, conf = out
        self.assertTrue(text and conf == 1.0)
        self.assertIsNone(p.summarize([]))

    def test_factory_selects_fake(self):
        self.assertIsInstance(make_summary_provider(_cfg()), FakeSummaryProvider)


class TestClustering(unittest.TestCase):
    def test_groups_by_tag_over_min(self):
        mems = [_mem(1, ["deploy"]), _mem(2, ["deploy"]), _mem(3, ["deploy"]),
                _mem(4, ["trivia"])]  # trivia has only 1 -> below min_cluster
        clusters = _clusters(mems, _cfg(summary_min_cluster=2))
        self.assertEqual([(t, [m["id"] for m in recs]) for t, recs in clusters],
                         [("deploy", [1, 2, 3])])

    def test_skips_semantic_and_summaries(self):
        mems = [_mem(1, ["x"], kind="semantic"),  # semantic: excluded
                _mem(2, ["x", "summary"]),         # already a summary: excluded
                _mem(3, ["x"])]                    # eligible episodic
        clusters = _clusters(mems, _cfg(summary_min_cluster=1))
        self.assertEqual([m["id"] for _, recs in clusters for m in recs], [3])

    def test_skips_high_importance(self):
        mems = [_mem(1, ["x"], importance=0.9), _mem(2, ["x"], importance=0.9)]
        self.assertEqual(_clusters(mems, _cfg(summary_min_cluster=1,
                                              summary_max_importance=0.6)), [])

    def test_clusters_are_disjoint(self):
        # a record with two tags lands in only the first (alphabetical) cluster
        mems = [_mem(1, ["a", "b"]), _mem(2, ["a"]), _mem(3, ["b"])]
        clusters = dict((t, [m["id"] for m in recs])
                        for t, recs in _clusters(mems, _cfg(summary_min_cluster=1)))
        placed = [i for ids in clusters.values() for i in ids]
        self.assertEqual(sorted(placed), [1, 2, 3])
        self.assertEqual(len(placed), len(set(placed)))  # no record placed twice

    def test_caps_clusters_per_run(self):
        mems = [_mem(i, [f"t{i//2}"]) for i in range(20)]
        clusters = _clusters(mems, _cfg(summary_min_cluster=2,
                                        summary_max_clusters_per_run=3))
        self.assertLessEqual(len(clusters), 3)


if __name__ == "__main__":
    unittest.main()