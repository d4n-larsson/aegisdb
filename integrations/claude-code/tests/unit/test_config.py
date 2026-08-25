"""Unit tests for config precedence and namespace resolution (T012)."""
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.config import (CONFIG_BASENAME, PROJECT_DIR, Config, ConfigError,
                              config_path, derive_namespace, env_for_explicit_root,
                              load_config, project_root, resolve_namespace)

# The pre-existing cases below call load_config without a cwd, which now
# discovers <cwd>/.aegisdb/config.json — and this repo is itself a consumer, so
# the moment anyone runs aegisdb-init here they would start reading real project
# settings and asserting against them. Point them at an empty directory instead.
_ISOLATED = tempfile.mkdtemp(prefix='aegis-test-noconfig-')


class TestConfig(unittest.TestCase):
    def test_defaults(self):
        cfg = load_config(env={"AEGIS_NAMESPACE": "x"}, cwd=_ISOLATED)
        self.assertEqual(cfg.aegis_port, 9470)
        self.assertEqual(cfg.recall_time_budget_ms, 800)
        self.assertTrue(cfg.recall_enabled)
        self.assertEqual(cfg.embedding_mode, "none")
        self.assertEqual(cfg.recall_max_chars_per_memory, 500)
        self.assertEqual(cfg.recall_char_budget, 2000)

    def test_recall_budget_env_coercion(self):
        cfg = load_config(env={
            "AEGIS_NAMESPACE": "x",
            "AEGIS_RECALL_MAX_CHARS_PER_MEMORY": "120",
            "AEGIS_RECALL_CHAR_BUDGET": "0",
        }, cwd=_ISOLATED)
        self.assertEqual(cfg.recall_max_chars_per_memory, 120)
        self.assertIsInstance(cfg.recall_max_chars_per_memory, int)
        self.assertEqual(cfg.recall_char_budget, 0)

    def test_env_overrides_and_coercion(self):
        cfg = load_config(env={
            "AEGIS_NAMESPACE": "x",
            "AEGIS_PORT": "1234",
            "AEGIS_RECALL_ENABLED": "false",
            "AEGIS_RECALL_MIN_SCORE": "0.5",
            "AEGIS_EMBEDDING_DIMENSIONS": "256",
        }, cwd=_ISOLATED)
        self.assertEqual(cfg.aegis_port, 1234)
        self.assertIsInstance(cfg.aegis_port, int)
        self.assertFalse(cfg.recall_enabled)
        self.assertEqual(cfg.recall_min_score, 0.5)
        self.assertEqual(cfg.embedding_dimensions, 256)

    def test_overrides_beat_env(self):
        cfg = load_config(env={"AEGIS_NAMESPACE": "x", "AEGIS_PORT": "1111"},
                          overrides={"aegis_port": 2222}, cwd=_ISOLATED)
        self.assertEqual(cfg.aegis_port, 2222)

    def test_voyage_key_selects_voyage_mode(self):
        cfg = load_config(env={"AEGIS_NAMESPACE": "x", "VOYAGE_API_KEY": "k"}, cwd=_ISOLATED)
        self.assertEqual(cfg.embedding_mode, "voyage")
        # explicit mode still wins
        cfg2 = load_config(env={"AEGIS_NAMESPACE": "x", "VOYAGE_API_KEY": "k",
                                "AEGIS_EMBEDDING_MODE": "none"}, cwd=_ISOLATED)
        self.assertEqual(cfg2.embedding_mode, "none")

    def test_auth_token_defaults_blank_and_reads_env(self):
        self.assertEqual(load_config(env={"AEGIS_NAMESPACE": "x"}, cwd=_ISOLATED).auth_token, "")
        cfg = load_config(env={"AEGIS_NAMESPACE": "x", "AEGIS_AUTH_TOKEN": "s3cret"}, cwd=_ISOLATED)
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

class TestCoercionCoverage(unittest.TestCase):
    """Every non-str field must appear in a coercion set.

    Two fields shipped without one: AEGIS_EXTRACT_TRIPLES=false coerced to the
    string "false", which is truthy, so explicitly disabling the feature turned
    it on; and extract_max_triples arrived as a str that raised TypeError the
    moment it met a comparison. Both are invisible until an operator sets the
    env var, so the guard is structural rather than per-field."""

    def test_every_non_string_field_is_coerced(self):
        import dataclasses
        from aegis_mcp import config as cfg
        typed = cfg._BOOL | cfg._INT | cfg._FLOAT
        missing = []
        for f in dataclasses.fields(cfg.Config):
            if f.type in ("bool", "int", "float") and f.name not in typed:
                missing.append(f.name)
        self.assertEqual(missing, [],
                         f"non-str Config fields absent from _BOOL/_INT/_FLOAT: "
                         f"{missing}")

    def test_a_false_string_disables_a_bool_field(self):
        from aegis_mcp.config import load_config
        for off in ("false", "0", "no", "off"):
            c = load_config(env={"AEGIS_EXTRACT_TRIPLES": off}, cwd=_ISOLATED)
            self.assertFalse(c.extract_triples, f"{off!r} should disable")


class TestProjectDirDiscovery(unittest.TestCase):
    """`.aegisdb/config.json` is found without anyone naming it.

    This is what makes one setting reach both callers: the MCP server gets env
    from `.mcp.json`, a Claude Code hook entry has no env field at all, and a
    file on disk is read by whoever runs next.
    """

    def _project(self, doc=None):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        if doc is not None:
            os.makedirs(os.path.join(d, PROJECT_DIR), exist_ok=True)
            with open(os.path.join(d, PROJECT_DIR, CONFIG_BASENAME), "w",
                      encoding="utf-8") as fh:
                json.dump(doc, fh)
        return d

    def test_default_path_is_project_local(self):
        d = self._project()
        self.assertEqual(config_path(env={}, cwd=d),
                         os.path.join(d, PROJECT_DIR, CONFIG_BASENAME))

    def test_claude_project_dir_wins_over_cwd(self):
        d, other = self._project(), self._project()
        self.assertEqual(config_path(env={"CLAUDE_PROJECT_DIR": d}, cwd=other),
                         os.path.join(d, PROJECT_DIR, CONFIG_BASENAME))

    def test_explicit_aegis_config_still_wins(self):
        d = self._project({"aegis_port": 1})
        elsewhere = os.path.join(self._project(), "other.json")
        self.assertEqual(config_path(env={"AEGIS_CONFIG": elsewhere}, cwd=d),
                         elsewhere)

    def test_absent_file_is_not_an_error(self):
        cfg = load_config(env={}, cwd=self._project())
        self.assertEqual(cfg.aegis_port, 9470)

    def test_discovered_file_is_loaded(self):
        d = self._project({"aegis_port": 9999, "recall_top_k": 2})
        cfg = load_config(env={}, cwd=d)
        self.assertEqual(cfg.aegis_port, 9999)
        self.assertEqual(cfg.recall_top_k, 2)

    def test_env_still_beats_the_file(self):
        d = self._project({"aegis_port": 9999})
        cfg = load_config(env={"AEGIS_PORT": "7000"}, cwd=d)
        self.assertEqual(cfg.aegis_port, 7000)

    def test_values_are_coerced_from_json_strings(self):
        d = self._project({"aegis_port": "9999", "recall_enabled": "false"})
        cfg = load_config(env={}, cwd=d)
        self.assertEqual(cfg.aegis_port, 9999)
        self.assertIsInstance(cfg.aegis_port, int)
        self.assertFalse(cfg.recall_enabled)

    def test_unknown_keys_are_ignored(self):
        d = self._project({"not_a_field": 1, "aegis_port": 9999})
        self.assertEqual(load_config(env={}, cwd=d).aegis_port, 9999)

    def test_pinned_namespace_beats_the_path_fallback(self):
        d = self._project({"namespace": "pinned"})
        self.assertEqual(load_config(env={}, cwd=d).namespace, "pinned")
        self.assertNotEqual(derive_namespace(env={}, cwd=d), "pinned")

    def test_env_namespace_still_beats_the_pinned_one(self):
        d = self._project({"namespace": "pinned"})
        cfg = load_config(env={"AEGIS_NAMESPACE": "from-env"}, cwd=d)
        self.assertEqual(cfg.namespace, "from-env")

    def test_a_broken_file_is_an_error_not_a_silent_default(self):
        d = self._project()
        os.makedirs(os.path.join(d, PROJECT_DIR), exist_ok=True)
        with open(os.path.join(d, PROJECT_DIR, CONFIG_BASENAME), "w",
                  encoding="utf-8") as fh:
            fh.write("{not json")
        with self.assertRaises(ConfigError) as ctx:
            load_config(env={}, cwd=d)
        self.assertIn(CONFIG_BASENAME, str(ctx.exception))

    def test_a_json_scalar_is_refused(self):
        d = self._project(["a", "list"])
        with self.assertRaises(ConfigError):
            load_config(env={}, cwd=d)


class TestFileIsASourceNotADefault(unittest.TestCase):
    """A setting chosen in the file must not be overridden by an inferred one."""

    def _project(self, doc):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        os.makedirs(os.path.join(d, PROJECT_DIR))
        with open(os.path.join(d, PROJECT_DIR, CONFIG_BASENAME), "w") as fh:
            json.dump(doc, fh)
        return d

    def test_a_voyage_key_does_not_override_a_file_set_mode(self):
        d = self._project({"embedding_mode": "local", "embedding_dimensions": 384})
        cfg = load_config(env={"VOYAGE_API_KEY": "k"}, cwd=d)
        self.assertEqual(cfg.embedding_mode, "local")
        self.assertEqual(cfg.embedding_dimensions, 384)

    def test_a_voyage_key_still_applies_when_nothing_chose_a_mode(self):
        d = self._project({"aegis_port": 1234})
        self.assertEqual(load_config(env={"VOYAGE_API_KEY": "k"}, cwd=d).embedding_mode,
                         "voyage")

    def test_a_bad_value_names_the_file_and_the_key(self):
        d = self._project({"aegis_port": "abc"})
        with self.assertRaises(ConfigError) as ctx:
            load_config(env={}, cwd=d)
        self.assertIn(CONFIG_BASENAME, str(ctx.exception))
        self.assertIn("aegis_port", str(ctx.exception))

    def test_a_null_value_is_a_config_error_not_a_typeerror(self):
        d = self._project({"recall_top_k": None})
        with self.assertRaises(ConfigError):
            load_config(env={}, cwd=d)


class TestExplicitRootEnv(unittest.TestCase):
    def test_it_drops_only_the_project_pointer(self):
        env = env_for_explicit_root({"CLAUDE_PROJECT_DIR": "/x", "AEGIS_PORT": "1"})
        self.assertNotIn("CLAUDE_PROJECT_DIR", env)
        self.assertEqual(env["AEGIS_PORT"], "1")

    def test_the_root_then_follows_the_given_cwd(self):
        with tempfile.TemporaryDirectory() as d:
            env = env_for_explicit_root({"CLAUDE_PROJECT_DIR": "/elsewhere"})
            self.assertEqual(project_root(env=env, cwd=d), os.path.abspath(d))
