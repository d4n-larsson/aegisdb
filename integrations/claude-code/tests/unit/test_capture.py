"""Unit tests for capture salience scoring and filtering (T032)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.capture import (score_text, derive_candidates, filter_candidates,
                               load_transcript)
from aegis_mcp.config import Config


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


if __name__ == "__main__":
    unittest.main()