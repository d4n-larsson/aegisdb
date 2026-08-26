"""Keyword-only recall against a real server.

The unit tests pin what expansion adds to a query. What they cannot show is the
thing it exists for: that with embeddings off — the default a new project gets —
an ordinary question now reaches a memory phrased slightly differently, through
the real BM25 index rather than a stub.
"""
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from harness import AegisServer, binary_available, make_config  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import aegis_mcp.tools as tools_mod  # noqa: E402
from aegis_mcp.client import AegisClient  # noqa: E402
from aegis_mcp.embeddings import NoneProvider  # noqa: E402
from aegis_mcp.recall import run_recall  # noqa: E402
from aegis_mcp.tools import MemoryTools  # noqa: E402

CORPUS = [
    "widgetco deploys with make ship",
    "postgres runs in docker on port 5544",
    "the proxy is guarded by an API token per tenant",
]


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestKeywordRecallE2E(unittest.TestCase):
    def _tools(self, srv):
        cfg = make_config(srv, embedding_mode="none", namespace="kw-e2e")
        t = MemoryTools(cfg, AegisClient.from_config(cfg), NoneProvider())
        for text in CORPUS:
            t.save(text, semantic=True)
        return cfg, t

    def _finds(self, t, question, wanted):
        got = [m["text"] for m in
               t.search(query=question, top_k=3, lexical=True).get("memories", [])]
        return wanted in got

    def test_a_question_reaches_a_memory_phrased_in_the_plural(self):
        """The reported failure, end to end: one letter of difference between
        "deploy" and "deploys" was the whole reason recall stayed empty."""
        with AegisServer() as srv:
            _, t = self._tools(srv)
            self.assertTrue(self._finds(t, "how do I deploy this project?",
                                        CORPUS[0]))

    def test_and_it_used_to_miss(self):
        """The bound that makes the test above mean something. Without
        expansion the same question finds nothing at all."""
        with AegisServer() as srv:
            _, t = self._tools(srv)
            with mock.patch.object(tools_mod, "expand_lexical_query",
                                   lambda q: q):
                self.assertFalse(self._finds(t, "how do I deploy this project?",
                                             CORPUS[0]))

    def test_an_exact_identifier_still_matches_exactly(self):
        """What expansion must not cost. The keyword path earns its place by
        finding a flag or a file:line verbatim."""
        with AegisServer() as srv:
            _, t = self._tools(srv)
            t.save("the cap lives in hnsw.c:214 behind --ann-threshold",
                   semantic=True)
            for token in ("hnsw.c:214", "--ann-threshold"):
                got = [m["text"] for m in
                       t.search(query=token, top_k=3,
                                lexical=True).get("memories", [])]
                self.assertEqual(len(got), 1, f"{token} matched {len(got)}")

    def test_an_off_topic_question_now_recalls_nothing(self):
        """The other half of the fix, and the one a session notices most: the
        index has no stopword list, so this question used to match a memory
        about an API token on the strength of "the" — and recall injected it,
        scored, into the turn."""
        with AegisServer() as srv:
            _, t = self._tools(srv)
            for q in ("what is the kubernetes ingress annotation?",
                      "is the weather nice today?"):
                res = t.search(query=q, top_k=3, lexical=True)
                self.assertEqual(res.get("memories"), [], q)

    def test_and_it_used_to_inject_something(self):
        """The bound on the test above."""
        with AegisServer() as srv:
            _, t = self._tools(srv)
            with mock.patch.object(tools_mod, "expand_lexical_query",
                                   lambda q: q):
                res = t.search(query="what is the kubernetes ingress annotation?",
                               top_k=3, lexical=True)
            self.assertTrue(res.get("memories"))

    def test_the_recall_hook_path_gets_the_same_benefit(self):
        """`run_recall` is what a turn actually goes through."""
        with AegisServer() as srv:
            cfg, _ = self._tools(srv)
            result = run_recall("how do I deploy this project?", cfg,
                                NoneProvider())
            self.assertIn("make ship", result.context)


if __name__ == "__main__":
    unittest.main()
