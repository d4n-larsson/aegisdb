"""End-to-end: background summarization distils a cluster, links provenance, and
archives the sources — against a real server, using the deterministic fake
summarizer (no external model)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from harness import AegisServer, binary_available, make_config  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from aegis_mcp.client import AegisClient  # noqa: E402
from aegis_mcp.embeddings import FakeProvider  # noqa: E402
from aegis_mcp.summary import FakeSummaryProvider  # noqa: E402
from aegis_mcp.summarize import run_summarize  # noqa: E402
from aegis_mcp.tools import MemoryTools  # noqa: E402


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestSummarizeE2E(unittest.TestCase):
    def _seed(self, srv):
        cfg = make_config(srv, summary_mode="fake", summary_min_age_ms=0,
                          summary_min_cluster=2, summary_max_importance=1.0)
        tools = MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port),
                            FakeProvider(srv.dim))
        ids = []
        for text in ("deploy runs make ship", "deploy failed on staging",
                     "fixed the flaky staging deploy step"):
            ids.append(tools.save(text, tags=["deploy"], importance=0.3)["id"])
        tools.save("the mascot is an otter", tags=["trivia"], importance=0.3)
        return cfg, tools, ids

    def test_distills_cluster_links_and_archives(self):
        with AegisServer() as srv:
            cfg, tools, ids = self._seed(srv)
            client = AegisClient(cfg.aegis_host, cfg.aegis_port)

            rep = run_summarize(cfg, client, FakeSummaryProvider(),
                                FakeProvider(srv.dim), now_ms=10**13)
            self.assertTrue(rep["ok"])
            self.assertEqual(rep["clusters"], 1)      # only "deploy" (>=2); trivia has 1
            self.assertEqual(rep["summarized"], 1)
            self.assertEqual(rep["archived"], 3)      # all 3 sources tombstoned

            # sources are gone from recall (tombstoned)
            for i in ids:
                self.assertFalse(tools.get(i).get("ok"),
                                 "summarized source should be archived")
            # a semantic summary tagged 'summary' now exists and is recallable
            found = tools.search(tags=["summary"], match="any")
            self.assertTrue(any("summary" in (m.get("tags") or [])
                                for m in found.get("memories", [])),
                            "a summary memory was written")

    def test_dry_run_writes_nothing(self):
        with AegisServer() as srv:
            cfg, tools, ids = self._seed(srv)
            client = AegisClient(cfg.aegis_host, cfg.aegis_port)
            rep = run_summarize(cfg, client, FakeSummaryProvider(),
                                FakeProvider(srv.dim), dry_run=True, now_ms=10**13)
            self.assertTrue(rep["ok"] and rep["dry_run"])
            self.assertEqual(rep["clusters"], 1)
            self.assertEqual(rep["summarized"], 0)     # nothing written
            self.assertTrue(rep["summaries"])          # but the plan is reported
            for i in ids:                              # sources untouched
                self.assertTrue(tools.get(i).get("ok"))

    def test_provider_none_is_a_noop(self):
        from aegis_mcp.summary import NoneSummaryProvider
        with AegisServer() as srv:
            cfg, tools, ids = self._seed(srv)
            client = AegisClient(cfg.aegis_host, cfg.aegis_port)
            rep = run_summarize(cfg, client, NoneSummaryProvider(),
                                FakeProvider(srv.dim), now_ms=10**13)
            self.assertFalse(rep["ok"])                # disabled -> no work
            self.assertTrue(tools.get(ids[0]).get("ok"))  # nothing archived


if __name__ == "__main__":
    unittest.main()