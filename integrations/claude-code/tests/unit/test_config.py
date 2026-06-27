"""Unit tests for config precedence and namespace resolution (T012)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.config import load_config, resolve_namespace, Config


class TestConfig(unittest.TestCase):
    def test_defaults(self):
        cfg = load_config(env={"AEGIS_NAMESPACE": "x"})
        self.assertEqual(cfg.aegis_port, 9470)
        self.assertEqual(cfg.recall_time_budget_ms, 800)
        self.assertTrue(cfg.recall_enabled)
        self.assertEqual(cfg.embedding_mode, "none")

    def test_env_overrides_and_coercion(self):
        cfg = load_config(env={
            "AEGIS_NAMESPACE": "x",
            "AEGIS_PORT": "1234",
            "AEGIS_RECALL_ENABLED": "false",
            "AEGIS_RECALL_MIN_SCORE": "0.5",
            "AEGIS_EMBEDDING_DIMENSIONS": "256",
        })
        self.assertEqual(cfg.aegis_port, 1234)
        self.assertIsInstance(cfg.aegis_port, int)
        self.assertFalse(cfg.recall_enabled)
        self.assertEqual(cfg.recall_min_score, 0.5)
        self.assertEqual(cfg.embedding_dimensions, 256)

    def test_overrides_beat_env(self):
        cfg = load_config(env={"AEGIS_NAMESPACE": "x", "AEGIS_PORT": "1111"},
                          overrides={"aegis_port": 2222})
        self.assertEqual(cfg.aegis_port, 2222)

    def test_voyage_key_selects_voyage_mode(self):
        cfg = load_config(env={"AEGIS_NAMESPACE": "x", "VOYAGE_API_KEY": "k"})
        self.assertEqual(cfg.embedding_mode, "voyage")
        # explicit mode still wins
        cfg2 = load_config(env={"AEGIS_NAMESPACE": "x", "VOYAGE_API_KEY": "k",
                                "AEGIS_EMBEDDING_MODE": "none"})
        self.assertEqual(cfg2.embedding_mode, "none")

    def test_auth_token_defaults_blank_and_reads_env(self):
        self.assertEqual(load_config(env={"AEGIS_NAMESPACE": "x"}).auth_token, "")
        cfg = load_config(env={"AEGIS_NAMESPACE": "x", "AEGIS_AUTH_TOKEN": "s3cret"})
        self.assertEqual(cfg.auth_token, "s3cret")

    def test_namespace_never_blank(self):
        cfg = load_config(env={}, cwd="/tmp/some/project")
        self.assertTrue(cfg.namespace)

    def test_namespace_precedence(self):
        self.assertEqual(resolve_namespace(explicit="explicit", env={"AEGIS_NAMESPACE": "envns"}),
                         "explicit")
        self.assertEqual(resolve_namespace(env={"AEGIS_NAMESPACE": "envns"}), "envns")

    def test_namespace_distinct_per_path(self):
        a = resolve_namespace(env={}, cwd="/home/u/projA")
        b = resolve_namespace(env={}, cwd="/home/u/projB")
        self.assertNotEqual(a, b)
        # stable for the same path
        self.assertEqual(a, resolve_namespace(env={}, cwd="/home/u/projA"))


if __name__ == "__main__":
    unittest.main()