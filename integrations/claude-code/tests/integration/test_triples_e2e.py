"""The seam, end to end against a real server (ROADMAP 5.4 PR 3).

These exist because PR 2's unit tests could not see its worst bug. The fake
returned raw cosines where the real search returns blended ones, so a threshold
that was mathematically unreachable in production looked reachable in the tests.
A fake kinder than production hides precisely the bug it exists to catch — so
the wiring is exercised against an actual aegisdb with an actual registry, and
the server gets the last word on every write.
"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from harness import AegisServer, binary_available, make_config  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from aegis_mcp.client import AegisClient  # noqa: E402
from aegis_mcp.embeddings import FakeProvider  # noqa: E402
from aegis_mcp.extract import FakeExtractionProvider, load_vocabulary  # noqa: E402
from aegis_mcp.tools import MemoryTools  # noqa: E402
from aegis_mcp.triples import store_triples  # noqa: E402

REGISTRY = {
    "part_of": {"object": "id", "transitive": True},
    "defaults_to": {"object": "string", "cardinality": "one"},
}


def _registry_file():
    fd, path = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w") as fh:
        json.dump(REGISTRY, fh)
    return path


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestTriplesE2E(unittest.TestCase):
    def setUp(self):
        self.registry = _registry_file()

    def tearDown(self):
        os.unlink(self.registry)

    def _wired(self, srv, **overrides):
        opts = {"extract_triples": True, "extract_registry": self.registry}
        opts.update(overrides)
        cfg = make_config(srv, **opts)
        tools = MemoryTools(cfg, AegisClient.from_config(cfg),
                            FakeProvider(srv.dim))
        return cfg, tools

    def test_a_transcript_becomes_facts_the_server_accepts(self):
        with AegisServer(extra_args=["--predicate-registry", self.registry]) as srv:
            cfg, tools = self._wired(srv)
            vocab = load_vocabulary(self.registry)
            res = store_triples(
                tools,
                "hnsw.c : part_of : the storage layer\n"
                "the recall hook : defaults_to : none\n",
                vocab, cfg, FakeExtractionProvider())

            self.assertEqual(res.stored, 2, f"{res}")
            self.assertEqual(res.failed, 0)
            self.assertEqual(res.in_vocabulary_rate, 1.0)
            # Entities had to be minted: the store was empty.
            self.assertEqual(res.entities_minted, 3)  # hnsw.c, layer, hook

            # And the server really holds them — the assertion the unit tests
            # cannot make, since they never see an insert refused.
            found = tools.search(tags=["fact"], top_k=10)
            self.assertTrue(found.get("ok"))
            self.assertEqual(len(found.get("memories", [])), 2)

    def test_an_out_of_vocabulary_predicate_is_dropped_not_written(self):
        with AegisServer(extra_args=["--predicate-registry", self.registry]) as srv:
            cfg, tools = self._wired(srv)
            vocab = load_vocabulary(self.registry)
            res = store_triples(
                tools,
                "hnsw.c : invented_by_the_model : the storage layer\n"
                "the recall hook : defaults_to : none\n",
                vocab, cfg, FakeExtractionProvider())

            self.assertEqual(res.proposed, 2)
            self.assertEqual(res.rejected, 1)
            self.assertEqual(res.stored, 1)
            self.assertEqual(res.in_vocabulary_rate, 0.5)
            self.assertEqual(res.failed, 0, "the rejected one never reached the server")

    def test_the_server_refuses_what_a_drifted_registry_would_allow(self):
        """The client-side check is an optimization; the server's is the
        contract. Started with a registry that omits `part_of`, the write must
        fail — and be counted as a failure rather than silently dropped."""
        narrow = tempfile.mkstemp(suffix=".json")[1]
        with open(narrow, "w") as fh:
            json.dump({"defaults_to": {"object": "string"}}, fh)
        try:
            with AegisServer(extra_args=["--predicate-registry", narrow]) as srv:
                cfg, tools = self._wired(srv)  # client vocab still has part_of
                vocab = load_vocabulary(self.registry)
                res = store_triples(tools,
                                    "hnsw.c : part_of : the storage layer\n",
                                    vocab, cfg, FakeExtractionProvider())
                self.assertEqual(res.stored, 0)
                self.assertEqual(res.failed, 1)
                self.assertEqual(res.rejected, 0, "client-side it looked fine")
        finally:
            os.unlink(narrow)

    def test_a_second_transcript_reuses_the_entities(self):
        """The grounding threshold has to be reachable against the *real*
        search, whose score is a blend rather than a cosine. This is the
        end-to-end form of the bug the PR 2 unit tests could not see."""
        with AegisServer(extra_args=["--predicate-registry", self.registry]) as srv:
            cfg, tools = self._wired(srv)
            vocab = load_vocabulary(self.registry)
            first = store_triples(tools, "hnsw.c : part_of : the storage layer\n",
                                  vocab, cfg, FakeExtractionProvider())
            self.assertEqual(first.entities_minted, 2)

            second = store_triples(tools,
                                   "hnsw.c : defaults_to : exact-scan\n",
                                   vocab, cfg, FakeExtractionProvider())
            self.assertEqual(second.stored, 1)
            self.assertEqual(second.entities_minted, 0,
                             "hnsw.c already exists and must be reused")
            self.assertEqual(second.entities_resolved, 1)

            entities = tools.search(tags=["entity"], top_k=20)
            texts = sorted(m["text"] for m in entities.get("memories", []))
            self.assertEqual(texts, ["hnsw.c", "the storage layer"])

    def test_a_paraphrase_resolves_through_the_cosine_path(self):
        """THE regression from PR 2, end to end against a real server.

        The exact pass cannot help here — the texts differ — so only the
        similarity path can resolve, and against the real search its score is
        `sim * (0.5 + 0.5 * importance) * confidence`. An entity minted at
        importance 0.5 therefore tops out at 0.75 even for a *perfect* match,
        so comparing the 0.85 floor against the blended value made reuse
        impossible at any realistic threshold. The unit tests could not see it
        because the fake handed back raw cosines.

        A word-order variant scores cosine 1.0 under FakeProvider and is not an
        exact match, which is exactly the case that used to mint.
        """
        with AegisServer(extra_args=["--predicate-registry", self.registry]) as srv:
            cfg, tools = self._wired(srv)
            vocab = load_vocabulary(self.registry)
            first = store_triples(tools, "the storage layer : defaults_to : none\n",
                                  vocab, cfg, FakeExtractionProvider())
            self.assertEqual(first.entities_minted, 1)

            second = store_triples(tools,
                                   "storage layer the : defaults_to : exact-scan\n",
                                   vocab, cfg, FakeExtractionProvider())
            self.assertEqual(second.entities_resolved, 1,
                             "a paraphrase must reuse the entity, not mint one")
            self.assertEqual(second.entities_minted, 0)

            entities = tools.search(tags=["entity"], top_k=20)
            self.assertEqual(len(entities.get("memories", [])), 1,
                             "one thing, one symbol")

    def test_off_by_default_changes_nothing(self):
        with AegisServer(extra_args=["--predicate-registry", self.registry]) as srv:
            cfg = make_config(srv)  # extract_triples defaults to False
            tools = MemoryTools(cfg, AegisClient.from_config(cfg),
                                FakeProvider(srv.dim))
            res = store_triples(tools, "hnsw.c : part_of : the storage layer\n",
                                load_vocabulary(self.registry), cfg,
                                FakeExtractionProvider())
            self.assertEqual((res.stored, res.proposed), (0, 0))
            self.assertEqual(tools.search(tags=["fact"], top_k=5)
                             .get("memories", []), [])

    def test_no_registry_stores_nothing(self):
        """The vocabulary is the contract. Proposing triples with nothing to
        check them against is not a smaller version of the feature."""
        with AegisServer() as srv:
            cfg, tools = self._wired(srv, extract_registry="")
            res = store_triples(tools, "hnsw.c : part_of : the storage layer\n",
                                None, cfg, FakeExtractionProvider())
            self.assertEqual(res.stored, 0)


if __name__ == "__main__":
    unittest.main()
