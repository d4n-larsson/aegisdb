"""US5 end-to-end: semantic ranking and graceful fallback (T042)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from harness import AegisServer, binary_available, make_config  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from aegis_mcp.client import AegisClient  # noqa: E402
from aegis_mcp.embeddings import FakeProvider, NoneProvider  # noqa: E402
from aegis_mcp.tools import MemoryTools  # noqa: E402


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestSemanticE2E(unittest.TestCase):
    def test_semantic_query_ranks_intended_memory_top(self):
        with AegisServer() as srv:
            cfg = make_config(srv, recall_min_score=0.0)
            provider = FakeProvider(srv.dim)
            t = MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port), provider)
            t.save("Deploy the project by running make ship in the terminal", semantic=True)
            t.save("Our office plant is a fern that needs weekly watering", semantic=True)

            res = t.search(query="how to deploy and run the project", top_k=2)
            self.assertTrue(res["ok"])
            self.assertFalse(res["degraded"])
            self.assertTrue(res["memories"])
            top = res["memories"][0]
            self.assertIn("deploy", top["text"].lower())
            self.assertIn("score", top)  # semantic results carry a score

    def test_none_mode_falls_back_to_tag_search(self):
        with AegisServer() as srv:
            cfg = make_config(srv)
            # NoneProvider -> no embeddings; semantic query degrades to tag/time.
            t = MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port), NoneProvider())
            t.save("deploy with make ship", tags=["ops"])
            res = t.search(query="deploy", tags=["ops"], top_k=5)
            self.assertTrue(res["ok"])
            self.assertTrue(res["degraded"])  # query given but no embeddings
            self.assertEqual(res["total"], 1)


if __name__ == "__main__":
    unittest.main()