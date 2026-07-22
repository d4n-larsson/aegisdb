"""US3 end-to-end: capture persists salient memories, recallable later (T034)."""
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
from aegis_mcp.tools import MemoryTools  # noqa: E402
from aegis_mcp.capture import run_capture  # noqa: E402


def _write_transcript(lines):
    fd, path = tempfile.mkstemp(suffix=".jsonl")
    with os.fdopen(fd, "w") as fh:
        for role, text in lines:
            fh.write(json.dumps({"type": role, "content": text}) + "\n")
    return path


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestCaptureE2E(unittest.TestCase):
    def test_capture_then_recall(self):
        with AegisServer() as srv:
            cfg = make_config(srv)
            provider = FakeProvider(srv.dim)
            transcript = _write_transcript([
                ("user", "how should we indent?"),
                ("assistant", "We decided to use tabs for indentation going forward."),
                ("user", "thanks"),
            ])
            try:
                stored = run_capture({"transcript_path": transcript}, cfg, provider)
                self.assertGreaterEqual(stored, 1)
            finally:
                os.remove(transcript)

            # The captured decision is recallable in a fresh client.
            tools = MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port), provider)
            res = tools.search(query="what indentation do we use?", top_k=5)
            self.assertTrue(res["ok"])
            self.assertTrue(any("tabs" in m["text"].lower() for m in res["memories"]))

    def test_extraction_stores_semantic_facts_recallable(self):
        """ROADMAP 2.1: with extract_mode set, capture distils the transcript into
        semantic facts that land in the DB and are recallable."""
        with AegisServer() as srv:
            cfg = make_config(srv, extract_mode="fake")
            provider = FakeProvider(srv.dim)
            transcript = _write_transcript([
                ("assistant", "We decided to deploy production via make ship."),
                ("assistant", "The database connection pool max is twenty."),
                ("user", "do not remember my secret api key sk-xyz"),  # ephemeral
            ])
            try:
                stored = run_capture({"transcript_path": transcript}, cfg, provider)
                self.assertGreaterEqual(stored, 2)
            finally:
                os.remove(transcript)

            tools = MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port), provider)
            # facts are stored as semantic (type visible via a broad search)
            res = tools.search(query="how do we deploy?", top_k=10)
            self.assertTrue(res["ok"])
            texts = " ".join(m["text"].lower() for m in res["memories"])
            self.assertIn("make ship", texts)
            # the ephemeral secret was dropped before extraction
            self.assertNotIn("secret api key", texts)

    def test_nonsalient_session_stores_nothing(self):
        with AegisServer() as srv:
            cfg = make_config(srv)
            transcript = _write_transcript([
                ("user", "hi"), ("assistant", "hello, how can I help?"),
                ("user", "ok thanks"),
            ])
            try:
                self.assertEqual(run_capture({"transcript_path": transcript}, cfg,
                                             FakeProvider(srv.dim)), 0)
            finally:
                os.remove(transcript)

    def test_capture_disabled(self):
        with AegisServer() as srv:
            cfg = make_config(srv, capture_enabled=False)
            transcript = _write_transcript([("assistant", "We decided to use tabs going forward.")])
            try:
                self.assertEqual(run_capture({"transcript_path": transcript}, cfg,
                                             FakeProvider(srv.dim)), 0)
            finally:
                os.remove(transcript)

    def test_backend_down_stores_nothing_no_raise(self):
        with AegisServer() as srv:
            cfg = make_config(srv)
        transcript = _write_transcript([("assistant", "We decided to use tabs going forward.")])
        try:
            # server already stopped -> 0 stored, no exception
            self.assertEqual(run_capture({"transcript_path": transcript}, cfg,
                                         FakeProvider(cfg.embedding_dimensions or 16)), 0)
        finally:
            os.remove(transcript)


if __name__ == "__main__":
    unittest.main()