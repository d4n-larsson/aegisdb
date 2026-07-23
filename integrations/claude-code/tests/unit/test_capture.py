"""Unit tests for capture salience scoring and filtering (T032)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.capture import (score_text, derive_candidates, filter_candidates,
                               load_transcript, run_capture, looks_like_code)
from aegis_mcp.config import Config
from aegis_mcp.embeddings import NoneProvider


class _StubClient:
    """Records the payloads run_capture sends; always acks with a fresh id."""

    def __init__(self):
        self.saved = []
        self._id = 0

    def request(self, payload, read_timeout_ms=None):
        if payload.get("operation") == "insert":
            self.saved.append(payload)
            self._id += 1
            return {"ok": True, "record": {"id": self._id, "type": payload["type"]}}
        return {"ok": True}


class TestCaptureScoring(unittest.TestCase):
    def test_salient_phrase_scores_high(self):
        c = score_text("We decided to use tabs for indentation going forward.")
        self.assertGreater(c.salience, 0.5)
        self.assertIn("decision", c.tags)
        self.assertFalse(c.ephemeral)

    def test_trivial_text_scores_zero(self):
        self.assertEqual(score_text("ok thanks").salience, 0.0)

    def test_ephemeral_detected(self):
        c = score_text("Remember this temporary token but don't remember it long term")
        self.assertTrue(c.ephemeral)

    def test_preference_tagging(self):
        c = score_text("The team always prefers spaces over tabs as a convention.")
        self.assertIn("preference", c.tags)


class TestCapturePipeline(unittest.TestCase):
    def setUp(self):
        self.cfg = Config(capture_min_salience=0.5)

    def test_derive_and_filter(self):
        texts = [
            "Hi there.",
            "We decided to deploy via make ship going forward.",
            "ignore this scratch note we will use later",  # ephemeral marker 'scratch'
            "The root cause was a stale cache; the fix was to clear it on boot.",
        ]
        cands = derive_candidates(texts, self.cfg)
        self.assertTrue(len(cands) >= 2)
        survivors = filter_candidates(cands, self.cfg)
        texts_out = [c.text for c in survivors]
        # salient, non-ephemeral kept
        self.assertTrue(any("make ship" in t for t in texts_out))
        self.assertTrue(any("root cause" in t for t in texts_out))
        # ephemeral dropped
        self.assertFalse(any("scratch" in c.text.lower() for c in survivors))

    def test_nonsalient_session_yields_nothing(self):
        texts = ["hello", "thanks", "looks good", "ok"]
        self.assertEqual(filter_candidates(derive_candidates(texts, self.cfg), self.cfg), [])

    def test_threshold_filters_low_salience(self):
        texts = ["Note that the build is green."]  # 'note that' (0.5) +0.1 boost = 0.6
        cands = derive_candidates(texts, self.cfg)
        self.assertEqual(cands[0].salience, 0.6)
        # a stricter threshold above the candidate's salience filters it out
        strict = Config(capture_min_salience=0.7)
        self.assertEqual(filter_candidates(cands, strict), [])
        # at/below its salience it is kept
        self.assertEqual(len(filter_candidates(cands, Config(capture_min_salience=0.6))), 1)

    def test_load_transcript_missing_file(self):
        self.assertEqual(load_transcript(None), [])
        self.assertEqual(load_transcript("/no/such/path.jsonl"), [])


class TestSkipCode(unittest.TestCase):
    """Code/tool-output must not be captured, even when it contains a salience
    marker as a substring (the bug: `position: fixed` -> "fixed")."""

    def test_flags_code_and_tool_output(self):
        for t in [
            '46\tcontent: ""; position: fixed; inset: 0; z-index: 0;',   # Read output + "fixed"
            '104\techo \'{"tags":["user","preference"]}\' | nc localhost 9470',  # "prefer"
            "$ make ship",
            "const x = () => foo.bar;",
            "  --brass: var(--accent);",
            "+    return AEGIS_OK;",
            "if (retention >= min) { keep(); }",
        ]:
            self.assertTrue(looks_like_code(t), t)

    def test_passes_prose(self):
        for t in [
            "We decided to deploy via make ship going forward.",
            "The team always prefers spaces over tabs as a convention.",
            "Root cause was a stale cache; clear it on boot.",  # one ';' is fine
            "Remember to rotate the signing keys weekly.",
        ]:
            self.assertFalse(looks_like_code(t), t)

    def test_derive_skips_code_with_marker_substring(self):
        # a CSS line containing "fixed" and a JSON line containing "preference"
        # must NOT become memories; the real decision beside them must.
        texts = [
            '46\tbody { position: fixed; }',
            'req: {"tags":["user","preference"],"data":"x"}',
            "We decided to use tabs going forward.",
        ]
        cands = derive_candidates(texts, Config())
        out = [c.text for c in cands]
        self.assertTrue(any("tabs going forward" in t for t in out))
        self.assertFalse(any("position: fixed" in t for t in out))
        self.assertFalse(any("preference" in t for t in out))


class TestCaptureExtraction(unittest.TestCase):
    """ROADMAP 2.1: with extract_mode set, run_capture distils the transcript via
    the extractor and stores SEMANTIC facts; ephemeral content is dropped first;
    extract_mode=none keeps the heuristic path."""

    def _transcript(self, lines):
        import json
        import tempfile
        fd, path = tempfile.mkstemp(suffix=".jsonl")
        with os.fdopen(fd, "w") as fh:
            for content in lines:
                fh.write(json.dumps({"content": content}) + "\n")
        self.addCleanup(lambda: os.path.exists(path) and os.remove(path))
        return path

    def test_extraction_stores_semantic_facts(self):
        path = self._transcript([
            "We decided to deploy via make ship going forward.",
            "The root cause was a stale cache; the fix was to clear it on boot.",
            "do not remember this secret token abc123",  # ephemeral -> dropped
        ])
        cfg = Config(extract_mode="fake", capture_enabled=True)
        client = _StubClient()
        n = run_capture({"transcript_path": path}, cfg, NoneProvider(), client)
        self.assertGreaterEqual(n, 2)
        self.assertEqual(len(client.saved), n)
        # facts are stored as semantic (participate in dedup/supersede; forget-protected)
        self.assertTrue(all(p["type"] == "semantic" for p in client.saved))
        self.assertTrue(all("confidence" in p for p in client.saved))
        # ephemeral content never reached the store
        joined = " ".join(p["data"].lower() for p in client.saved)
        self.assertNotIn("secret token", joined)

    def test_none_mode_uses_heuristic_episodic(self):
        path = self._transcript([
            "We decided to deploy via make ship going forward.",
        ])
        cfg = Config(extract_mode="none", capture_enabled=True)
        client = _StubClient()
        n = run_capture({"transcript_path": path}, cfg, NoneProvider(), client)
        self.assertEqual(n, 1)
        # heuristic path stores episodic memories
        self.assertEqual(client.saved[0]["type"], "episodic")

    def test_capture_disabled_stores_nothing(self):
        path = self._transcript(["We decided to use tabs going forward."])
        cfg = Config(extract_mode="fake", capture_enabled=False)
        client = _StubClient()
        self.assertEqual(run_capture({"transcript_path": path}, cfg, NoneProvider(), client), 0)
        self.assertEqual(client.saved, [])


if __name__ == "__main__":
    unittest.main()