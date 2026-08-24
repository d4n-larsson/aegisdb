"""The read path against a real server (ROADMAP 5.4 PR 5, contract test 8).

The unit tests script the server's answers. What they cannot check is the two
things this PR actually claims: that `subsume` reaches a fact about a component
when the question named a layer — the multi-hop result, produced here by a real
inference job rather than a stub — and that verbalizing a derivation leaves the
derivation alone, including on disk.
"""
import json
import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from harness import AegisServer, binary_available, make_config  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from aegis_mcp.ask import ask, verbalize, verbalize_all  # noqa: E402
from aegis_mcp.client import AegisClient  # noqa: E402
from aegis_mcp.embeddings import FakeProvider  # noqa: E402
from aegis_mcp.extract import (FakeExtractionProvider,  # noqa: E402
                               load_vocabulary)
from aegis_mcp.tools import MemoryTools  # noqa: E402

REGISTRY = {
    "is_a": {"object": "id", "transitive": True},
    "caps_at": {"object": "string", "cardinality": "one"},
}


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestAskE2E(unittest.TestCase):
    def setUp(self):
        fd, self.registry = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w") as fh:
            json.dump(REGISTRY, fh)
        self.addCleanup(os.unlink, self.registry)
        self.fake = FakeExtractionProvider()

    def _wired(self, srv, **overrides):
        opts = {"ask_pattern": True, "ask_verbalize": True,
                "extract_registry": self.registry}
        opts.update(overrides)
        cfg = make_config(srv, **opts)
        client = AegisClient.from_config(cfg)
        return cfg, MemoryTools(cfg, client, FakeProvider(cfg))

    def _seed(self, tools):
        """A two-hop chain: the loop is in hnsw.c, hnsw.c is in storage, and
        only the *loop* carries the cap. No record names both the layer and the
        number, so retrieval cannot answer and only `subsume` can."""
        ids = {}
        for key, text in (("storage", "The storage layer."),
                          ("hnsw", "hnsw.c, the index."),
                          ("loop", "The neighbour-selection loop.")):
            ids[key] = tools.save(text, tags=["entity"], semantic=True)["id"]
        tools.save("hnsw.c belongs to the storage layer.", tags=["fact"],
                   semantic=True,
                   fact={"s": ids["hnsw"], "p": "is_a", "o": {"id": ids["storage"]}})
        tools.save("The neighbour loop sits inside hnsw.c.", tags=["fact"],
                   semantic=True,
                   fact={"s": ids["loop"], "p": "is_a", "o": {"id": ids["hnsw"]}})
        tools.save("The neighbour loop stops after 64 candidates.",
                   tags=["fact"], semantic=True,
                   fact={"s": ids["loop"], "p": "caps_at", "o": "64"})
        return ids

    def _await_derived(self, tools, subject_id, timeout=20.0):
        """Wait for the inference job to materialize the transitive closure."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            res = tools.search(pattern={"s": subject_id, "p": "is_a"},
                               top_k=10, derivations=True)
            derived = [m for m in res.get("memories", []) if m.get("derivation")]
            if derived:
                return derived
            time.sleep(0.5)
        return []

    def _server(self):
        return AegisServer(extra_args=[
            "--predicate-registry", self.registry, "--inference",
            "--inference-interval-sec", "1"])

    def test_a_question_about_a_layer_reaches_a_fact_about_a_component(self):
        """The multi-hop result, through the read path. The answer record never
        names the storage layer, so a prose search for the question cannot find
        it; `subsume` walks the derived `is_a` closure and does."""
        with self._server() as srv:
            cfg, tools = self._wired(srv)
            ids = self._seed(tools)
            self.assertTrue(self._await_derived(tools, ids["loop"]),
                            "inference produced no closure")

            vocab = load_vocabulary(self.registry)
            res = ask(tools, "The storage layer. ? caps_at", vocab, cfg,
                      self.fake, top_k=5)
            self.assertTrue(res["symbolic"], res)
            self.assertEqual(res["pattern"], {"s": ids["storage"],
                                              "p": "caps_at"})
            self.assertIn("64", " ".join(m["text"] for m in res["memories"]))

    def test_the_same_question_as_prose_finds_nothing(self):
        """The bound that makes the test above mean anything. If retrieval can
        answer it too, the symbolic path proved nothing."""
        with self._server() as srv:
            cfg, tools = self._wired(srv, ask_pattern=False)
            self._seed(tools)
            res = tools.search(query="what does the storage layer cap at?",
                               top_k=5, lexical=True)
            self.assertNotIn("64", " ".join(m["text"] for m in
                                            res.get("memories", [])))

    def test_verbalizing_leaves_the_derivation_untouched_on_disk(self):
        """Contract test 8. Two backends render one proof two ways; the
        `explain.derivation` the server returns is identical before, between
        and after — including on a fresh read, so nothing wrote back."""
        with self._server() as srv:
            cfg, tools = self._wired(srv)
            ids = self._seed(tools)
            derived = self._await_derived(tools, ids["loop"])
            self.assertTrue(derived, "inference produced no closure")
            mem = derived[0]
            before = json.dumps(mem["derivation"], sort_keys=True)

            class _Terse:
                def verbalize(self, claim, rule, premises):
                    return f"{rule} over {len(premises)}"

            a = verbalize(tools, mem, cfg, self.fake)
            b = verbalize(tools, mem, cfg, _Terse())
            self.assertTrue(a and b)
            self.assertNotEqual(a, b, "the rendering must depend on the backend")
            self.assertEqual(json.dumps(mem["derivation"], sort_keys=True),
                             before)

            fresh = tools.get(mem["id"])
            self.assertTrue(fresh.get("ok"))
            again = tools.search(pattern={"s": ids["loop"], "p": "is_a"},
                                 top_k=10, derivations=True)
            reread = [m for m in again["memories"] if m["id"] == mem["id"]][0]
            self.assertEqual(json.dumps(reread["derivation"], sort_keys=True),
                             before)

    def test_the_rendering_is_attached_beside_the_proof(self):
        with self._server() as srv:
            cfg, tools = self._wired(srv)
            ids = self._seed(tools)
            self.assertTrue(self._await_derived(tools, ids["loop"]))
            res = tools.search(pattern={"s": ids["loop"], "p": "is_a"},
                               top_k=10, derivations=True)
            out = verbalize_all(tools, res, cfg, self.fake)
            rendered = [m for m in out["memories"] if m.get("because")]
            self.assertTrue(rendered)
            for m in rendered:
                self.assertIn("derivation", m)
                self.assertIn("premises", m["derivation"]["routes"][0])

    def test_off_by_default_changes_nothing(self):
        """`ask_pattern` off must leave search byte-identical to today's."""
        with self._server() as srv:
            cfg, tools = self._wired(srv, ask_pattern=False,
                                     ask_verbalize=False)
            self._seed(tools)
            vocab = load_vocabulary(self.registry)
            asked = ask(tools, "The storage layer. ? caps_at", vocab, cfg,
                        self.fake, top_k=5)
            plain = tools.search(query="The storage layer. ? caps_at", top_k=5,
                                 lexical=True)
            self.assertFalse(asked["symbolic"])
            self.assertEqual([m["id"] for m in asked["memories"]],
                             [m["id"] for m in plain.get("memories", [])])


if __name__ == "__main__":
    unittest.main()
