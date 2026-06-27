"""Contract tests for the MCP tool result schemas (T020).

Validates the structured shapes in contracts/mcp-tools.md for the functions that
back the MCP tools (server.py binds these 1:1).
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from harness import AegisServer, binary_available, make_config  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from aegis_mcp.client import AegisClient  # noqa: E402
from aegis_mcp.embeddings import FakeProvider  # noqa: E402
from aegis_mcp.tools import MemoryTools  # noqa: E402


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestMcpToolContracts(unittest.TestCase):
    def _tools(self, srv):
        cfg = make_config(srv)
        return MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port),
                           FakeProvider(srv.dim))

    def test_save_success_schema(self):
        with AegisServer() as srv:
            r = self._tools(srv).save("hello", tags=["t"])
            self.assertEqual(set(r) >= {"ok", "id", "kind"}, True)
            self.assertTrue(r["ok"])
            self.assertIsInstance(r["id"], int)
            self.assertEqual(r["kind"], "episodic")

    def test_save_invalid_schema(self):
        with AegisServer() as srv:
            r = self._tools(srv).save("")
            self.assertFalse(r["ok"])
            self.assertEqual(r["error"], "invalid")
            self.assertIn("message", r)

    def test_search_result_schema(self):
        with AegisServer() as srv:
            t = self._tools(srv)
            t.save("alpha", tags=["g"])
            r = t.search(tags=["g"], top_k=5)
            self.assertTrue(r["ok"])
            self.assertIn("total", r)
            self.assertIn("memories", r)
            self.assertIsInstance(r["memories"], list)
            self.assertEqual(r["total"], len(r["memories"]))
            mem = r["memories"][0]
            self.assertEqual(set(mem) >= {"id", "text", "kind", "tags"}, True)

    def test_search_requires_a_filter(self):
        with AegisServer() as srv:
            r = self._tools(srv).search()
            self.assertFalse(r["ok"])
            self.assertEqual(r["error"], "invalid")

    def test_get_not_found_schema(self):
        with AegisServer() as srv:
            r = self._tools(srv).get(123456)
            self.assertFalse(r["ok"])
            self.assertEqual(r["error"], "not_found")

    def test_update_immutable_schema(self):
        with AegisServer() as srv:
            t = self._tools(srv)
            mem_id = t.save("episodic")["id"]
            r = t.update(mem_id, text="x")
            self.assertEqual(r["error"], "immutable")

    def test_all_tools_unavailable_when_backend_down(self):
        with AegisServer() as srv:
            cfg = make_config(srv)
        t = MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port,
                                         connect_timeout_ms=200), FakeProvider(16))
        for r in (t.save("x"), t.get(1), t.update(1, text="y"),
                  t.relate(1, 2), t.search(tags=["x"])):
            self.assertFalse(r["ok"])
            self.assertEqual(r["error"], "unavailable")


if __name__ == "__main__":
    unittest.main()