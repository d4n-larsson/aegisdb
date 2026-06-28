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


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestEnforcedIsolationE2E(unittest.TestCase):
    """Server-enforced multi-tenancy: each token is bound to a namespace + scope,
    so isolation holds even against an authenticated client that asks for another
    namespace — unlike advisory (no-auth) isolation."""

    TOKENS = ["acme-key acme rw", "beta-key beta rw", "acme-view acme ro"]

    def _tools(self, srv, token, namespace):
        cfg = make_config(srv, namespace=namespace, auth_token=token)
        return MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port,
                                            auth_token=cfg.auth_token),
                           FakeProvider(srv.dim))

    def test_token_namespace_is_authoritative(self):
        with AegisServer(token_lines=self.TOKENS) as srv:
            # acme's client deliberately claims a *different* namespace; the
            # server must pin the record to the token's namespace ("acme").
            acme = self._tools(srv, "acme-key", namespace="not-acme")
            beta = self._tools(srv, "beta-key", namespace="beta")

            mem_id = acme.save("acme only", tags=["s"])["id"]

            # A second authenticated tenant cannot read it (NOT_FOUND, no leak).
            self.assertEqual(beta.get(mem_id)["error"], "not_found")
            self.assertEqual(beta.search(tags=["s"], top_k=10)["total"], 0)

            # The owner sees it despite the client-side namespace mismatch,
            # proving the token's namespace — not AEGIS_NAMESPACE — governs.
            self.assertTrue(acme.get(mem_id)["ok"])
            self.assertEqual(acme.search(tags=["s"], top_k=10)["total"], 1)

    def test_read_only_token_cannot_write(self):
        with AegisServer(token_lines=self.TOKENS) as srv:
            ro = self._tools(srv, "acme-view", namespace="acme")
            res = ro.save("should be rejected", tags=["s"])
            self.assertFalse(res["ok"])
            self.assertEqual(res["error"], "forbidden")

    def test_missing_token_is_unauthorized(self):
        with AegisServer(token_lines=self.TOKENS) as srv:
            cfg = make_config(srv, namespace="acme")  # no auth_token
            tools = MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port),
                                FakeProvider(srv.dim))
            res = tools.save("no token", tags=["s"])
            self.assertFalse(res["ok"])
            self.assertEqual(res["error"], "unauthorized")


if __name__ == "__main__":
    unittest.main()