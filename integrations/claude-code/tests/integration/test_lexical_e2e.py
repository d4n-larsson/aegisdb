"""End-to-end: the integration's use of server-side lexical search (ROADMAP 4.1).

The case that matters most here is `embedding_mode=none` — the recall hook's
default. Before lexical search that configuration had *no* content-based
retrieval at all: a query was dropped and the search came back `degraded`. These
tests pin the new behaviour, and pin that the old behaviour still holds for
callers that did not opt in (capture's supersede detection filters on a cosine
floor and would break if handed fused scores).
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from harness import AegisServer, binary_available, make_config  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from aegis_mcp.client import AegisClient  # noqa: E402
from aegis_mcp.embeddings import FakeProvider, NoneProvider  # noqa: E402
from aegis_mcp.tools import MemoryTools  # noqa: E402

FLAG = "Per-tenant caps are set with --tenant-max-records at startup"
FERN = "Our office plant is a fern that needs weekly watering"


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestLexicalE2E(unittest.TestCase):
    def _tools(self, srv, provider, **cfg_over):
        cfg = make_config(srv, **cfg_over)
        return MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port),
                           provider)

    # --- the flagship case: no embeddings at all ---------------------------

    def test_no_embeddings_finds_exact_identifier(self):
        with AegisServer() as srv:
            t = self._tools(srv, NoneProvider())
            t.save(FLAG, semantic=True)
            t.save(FERN, semantic=True)

            res = t.search(query="--tenant-max-records", top_k=5, lexical=True)
            self.assertTrue(res["ok"], res)
            self.assertFalse(res["degraded"],
                             "lexical retrieval happened, so not degraded")
            self.assertTrue(res["memories"], "the identifier must be findable")
            self.assertIn("--tenant-max-records", res["memories"][0]["text"])

    def test_no_embeddings_without_optin_still_degrades(self):
        """The default is unchanged, which is what keeps capture's cosine-floor
        supersede logic safe."""
        with AegisServer() as srv:
            t = self._tools(srv, NoneProvider())
            t.save(FLAG, semantic=True)

            res = t.search(query="--tenant-max-records", top_k=5)
            self.assertTrue(res["ok"], res)
            self.assertTrue(res["degraded"],
                           "no embeddings and no opt-in => no content retrieval")

    def test_lexical_miss_is_empty_not_an_error(self):
        with AegisServer() as srv:
            t = self._tools(srv, NoneProvider())
            t.save(FLAG, semantic=True)
            res = t.search(query="zzz_nothing_matches", top_k=5, lexical=True)
            self.assertTrue(res["ok"], res)
            self.assertEqual(res["memories"], [])
            self.assertFalse(res["degraded"])

    # --- hybrid -----------------------------------------------------------

    def test_hybrid_carries_server_score_and_order(self):
        with AegisServer() as srv:
            t = self._tools(srv, FakeProvider(srv.dim), recall_min_score=0.0)
            t.save(FLAG, semantic=True)
            t.save(FERN, semantic=True)

            res = t.search(query="--tenant-max-records", top_k=5, lexical=True)
            self.assertTrue(res["ok"], res)
            self.assertFalse(res["degraded"])
            self.assertTrue(res["memories"])
            top = res["memories"][0]
            self.assertIn("--tenant-max-records", top["text"])
            # The score comes from the server's explain block (a fused
            # reciprocal-rank value), not a client-side cosine.
            self.assertIn("score", top)
            self.assertIsNotNone(top["score"])

    def test_hybrid_still_answers_a_topical_query(self):
        """Opting into lexical must not cost the semantic behaviour: a paraphrase
        with no shared rare token should still retrieve the right memory."""
        with AegisServer() as srv:
            t = self._tools(srv, FakeProvider(srv.dim), recall_min_score=0.0)
            t.save("Deploy the project by running make ship in the terminal",
                   semantic=True)
            t.save(FERN, semantic=True)

            res = t.search(query="how do I deploy and run the project", top_k=2,
                           lexical=True)
            self.assertTrue(res["ok"], res)
            self.assertTrue(res["memories"])
            self.assertIn("deploy", res["memories"][0]["text"].lower())

    # --- a server built without the index ---------------------------------

    def test_falls_back_when_server_has_no_lexical_index(self):
        """--no-lexical-index answers a `query` with NOT_READY. Recall must not
        break: the client retries without it and reports the pre-4.1 behaviour."""
        with AegisServer(extra_args=["--no-lexical-index"]) as srv:
            t = self._tools(srv, FakeProvider(srv.dim), recall_min_score=0.0)
            t.save("Deploy the project by running make ship", semantic=True)

            res = t.search(query="how do I deploy the project", top_k=5,
                           lexical=True)
            self.assertTrue(res["ok"], "NOT_READY must not surface as a failure")
            self.assertFalse(res["degraded"], "the embedding path still worked")
            self.assertTrue(res["memories"])

    def test_no_lexical_index_and_no_embeddings_degrades_cleanly(self):
        with AegisServer(extra_args=["--no-lexical-index"]) as srv:
            t = self._tools(srv, NoneProvider())
            t.save(FLAG, tags=["ops"], semantic=True)

            res = t.search(query="--tenant-max-records", top_k=5, lexical=True)
            self.assertTrue(res["ok"], "still no error, just nothing to rank on")
            self.assertTrue(res["degraded"])
            # Tag recall is unaffected by any of this.
            res = t.search(query="anything", tags=["ops"], top_k=5, lexical=True)
            self.assertTrue(res["ok"])
            self.assertTrue(res["memories"])


if __name__ == "__main__":
    unittest.main()