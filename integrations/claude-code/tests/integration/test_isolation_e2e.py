"""US4 end-to-end: per-project namespace isolation (T037, SC-004)."""
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
class TestIsolationE2E(unittest.TestCase):
    def _tools(self, srv, namespace):
        cfg = make_config(srv, namespace=namespace)
        return MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port),
                           FakeProvider(srv.dim))

    def test_cross_namespace_isolation(self):
        with AegisServer() as srv:
            proj_a = self._tools(srv, "project-A")
            proj_b = self._tools(srv, "project-B")

            mem_id = proj_a.save("secret to project A", tags=["a"])["id"]

            # B cannot get A's memory by id.
            self.assertEqual(proj_b.get(mem_id)["error"], "not_found")
            # B's tag search does not return A's memory.
            res_b = proj_b.search(tags=["a"], top_k=10)
            self.assertEqual(res_b["total"], 0)
            # A still sees its own memory.
            self.assertTrue(proj_a.get(mem_id)["ok"])
            res_a = proj_a.search(tags=["a"], top_k=10)
            self.assertEqual(res_a["total"], 1)

    def test_each_namespace_sees_only_its_own(self):
        with AegisServer() as srv:
            a = self._tools(srv, "ns-a")
            b = self._tools(srv, "ns-b")
            a.save("alpha one", tags=["x"])
            a.save("alpha two", tags=["x"])
            b.save("beta one", tags=["x"])
            self.assertEqual(a.search(tags=["x"], top_k=10)["total"], 2)
            self.assertEqual(b.search(tags=["x"], top_k=10)["total"], 1)


if __name__ == "__main__":
    unittest.main()