"""US2 end-to-end: automatic recall surfaces memories and respects budget (T027)."""
import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from harness import AegisServer, binary_available, make_config  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from aegis_mcp.client import AegisClient  # noqa: E402
from aegis_mcp.embeddings import FakeProvider  # noqa: E402
from aegis_mcp.tools import MemoryTools  # noqa: E402
from aegis_mcp.recall import run_recall  # noqa: E402


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestRecallE2E(unittest.TestCase):
    def test_recall_surfaces_related_memory(self):
        with AegisServer() as srv:
            cfg = make_config(srv)
            provider = FakeProvider(srv.dim)
            tools = MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port), provider)
            tools.save("Deploy the project by running make ship", tags=["ops"])
            tools.save("The mascot is a friendly otter", tags=["trivia"])

            result = run_recall("how do I deploy this project?", cfg, provider)
            self.assertFalse(result.degraded)
            self.assertTrue(result.memories)
            self.assertIn("make ship", result.context)
            # The deploy memory should outrank the trivia memory.
            self.assertIn("deploy", result.memories[0]["text"].lower())

    def test_recall_empty_when_no_memories(self):
        with AegisServer() as srv:
            cfg = make_config(srv)
            result = run_recall("anything", cfg, FakeProvider(srv.dim))
            self.assertEqual(result.memories, [])
            self.assertEqual(result.context, "")

    def test_recall_respects_time_budget_against_slow_backend(self):
        # A fake server that accepts but never replies -> recall must give up
        # within the budget and return a degraded, empty result.
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(4)
        port = listener.getsockname()[1]
        stop = threading.Event()

        def serve():
            conns = []
            while not stop.is_set():
                try:
                    listener.settimeout(0.2)
                    c, _ = listener.accept()
                    conns.append(c)  # hold open, never reply
                except (OSError, socket.timeout):
                    continue
            for c in conns:
                c.close()

        th = threading.Thread(target=serve, daemon=True)
        th.start()
        try:
            from aegis_mcp.config import load_config
            cfg = load_config(env={
                "AEGIS_HOST": "127.0.0.1", "AEGIS_PORT": str(port),
                "AEGIS_NAMESPACE": "slow", "AEGIS_RECALL_TIME_BUDGET_MS": "300",
            })
            start = time.monotonic()
            result = run_recall("deploy?", cfg, FakeProvider(16))
            elapsed = time.monotonic() - start
            self.assertTrue(result.degraded)
            self.assertEqual(result.memories, [])
            self.assertLess(elapsed, 1.0)  # bounded by the 300ms budget, not hung
        finally:
            stop.set()
            listener.close()


if __name__ == "__main__":
    unittest.main()