"""US1 end-to-end: explicit save / get / search against a real aegisdb (T021)."""
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
class TestToolsE2E(unittest.TestCase):
    def _tools(self, server, provider=None, **cfg_over):
        cfg = make_config(server, **cfg_over)
        client = AegisClient(cfg.aegis_host, cfg.aegis_port)
        return MemoryTools(cfg, client, provider or FakeProvider(server.dim))

    def test_save_then_get_across_clients(self):
        with AegisServer() as srv:
            saver = self._tools(srv)
            r = saver.save("User prefers tabs", tags=["style"], importance=0.7)
            self.assertTrue(r["ok"], r)
            mem_id = r["id"]

            # A separate client instance (simulating a new session) reads it back.
            reader = self._tools(srv)
            g = reader.get(mem_id)
            self.assertTrue(g["ok"])
            self.assertEqual(g["memory"]["text"], "User prefers tabs")
            self.assertEqual(g["memory"]["tags"], ["style"])

    def test_get_not_found(self):
        with AegisServer() as srv:
            t = self._tools(srv)
            self.assertEqual(t.get(999999)["error"], "not_found")

    def test_save_rejects_empty_text(self):
        with AegisServer() as srv:
            t = self._tools(srv)
            self.assertEqual(t.save("   ")["error"], "invalid")

    def test_search_by_tag(self):
        with AegisServer() as srv:
            t = self._tools(srv)
            t.save("deploy with make ship", tags=["ops"])
            t.save("unrelated note", tags=["misc"])
            res = t.search(tags=["ops"], top_k=10)
            self.assertTrue(res["ok"])
            self.assertEqual(res["total"], 1)
            self.assertIn("make ship", res["memories"][0]["text"])

    def test_episodic_update_is_immutable(self):
        with AegisServer() as srv:
            t = self._tools(srv)
            mem_id = t.save("an episodic fact")["id"]
            self.assertEqual(t.update(mem_id, text="changed")["error"], "immutable")

    def test_semantic_update(self):
        with AegisServer() as srv:
            t = self._tools(srv)
            mem_id = t.save("sky is blue", semantic=True)["id"]
            u = t.update(mem_id, text="sky is azure", confidence=0.9)
            self.assertTrue(u["ok"])
            self.assertEqual(u["memory"]["text"], "sky is azure")

    def test_relate(self):
        with AegisServer() as srv:
            t = self._tools(srv)
            a = t.save("fact A", semantic=True)["id"]
            b = t.save("fact B", semantic=True)["id"]
            r = t.relate(a, b, kind="derived_from")
            self.assertTrue(r["ok"])
            self.assertEqual(r["relationship"]["kind"], "derived_from")

    def test_backend_down_is_unavailable(self):
        # Start and immediately stop the server, then exercise the tools.
        with AegisServer() as srv:
            cfg = make_config(srv)
        t = MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port,
                                         connect_timeout_ms=200),
                        NoneProvider())
        self.assertEqual(t.save("x")["error"], "unavailable")
        self.assertEqual(t.get(1)["error"], "unavailable")
        s = t.search(tags=["x"])
        self.assertEqual(s["error"], "unavailable")
        self.assertEqual(s["memories"], [])


if __name__ == "__main__":
    unittest.main()