"""Unit tests for the aegisdb-init scaffolder: config building, non-destructive
merges, and idempotent file writes."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.config import (CONFIG_BASENAME, PROJECT_DIR, derive_namespace,
                              load_config)
from aegis_mcp.init import (GITIGNORE_LINE, build_mcp_config,
                            build_project_config, ensure_gitignore, main,
                            merge_hooks, merge_mcp, merge_project_config,
                            CAPTURE_CMD, RECALL_CMD)


class TestBuildMcpConfig(unittest.TestCase):
    def test_minimal_omits_empty_env(self):
        e = build_mcp_config(host="h", port=9470)
        self.assertEqual(e["command"], "uvx")
        self.assertEqual(e["args"], ["aegisdb-mcp"])
        self.assertEqual(e["env"], {"AEGIS_HOST": "h", "AEGIS_PORT": "9470"})
        # blank namespace/token/none-embedding contribute nothing
        for k in ("AEGIS_NAMESPACE", "AEGIS_AUTH_TOKEN", "AEGIS_EMBEDDING_MODE"):
            self.assertNotIn(k, e["env"])

    def test_full_env(self):
        e = build_mcp_config(host="memory.internal", port=9470, namespace="",
                             auth_token="tok", embedding_mode="voyage",
                             embedding_dim=1024)
        self.assertEqual(e["env"]["AEGIS_AUTH_TOKEN"], "tok")
        self.assertEqual(e["env"]["AEGIS_EMBEDDING_MODE"], "voyage")
        self.assertEqual(e["env"]["AEGIS_EMBEDDING_DIMENSIONS"], "1024")
        self.assertNotIn("AEGIS_NAMESPACE", e["env"])  # blank namespace omitted


class TestMergeMcp(unittest.TestCase):
    def test_add_preserves_other_servers(self):
        existing = {"mcpServers": {"other": {"command": "x"}}}
        entry = build_mcp_config(host="h", port=1)
        out, status = merge_mcp(existing, entry, force=False)
        self.assertEqual(status, "added")
        self.assertIn("other", out["mcpServers"])       # untouched
        self.assertEqual(out["mcpServers"]["memory"], entry)

    def test_existing_without_force_is_reported(self):
        existing = {"mcpServers": {"memory": {"command": "old"}}}
        out, status = merge_mcp(existing, build_mcp_config(host="h", port=1),
                                force=False)
        self.assertEqual(status, "conflict")
        self.assertEqual(out["mcpServers"]["memory"], {"command": "old"})  # not changed

    def test_force_overwrites(self):
        existing = {"mcpServers": {"memory": {"command": "old"}}}
        entry = build_mcp_config(host="h", port=1)
        out, status = merge_mcp(existing, entry, force=True)
        self.assertEqual(status, "updated")
        self.assertEqual(out["mcpServers"]["memory"], entry)

    def test_identical_entry_is_noop_even_with_force(self):
        entry = build_mcp_config(host="h", port=1)
        out, status = merge_mcp({"mcpServers": {"memory": entry}}, entry, force=True)
        self.assertEqual(status, "unchanged")


class TestMergeHooks(unittest.TestCase):
    def test_adds_both_hooks_to_empty(self):
        out, added = merge_hooks({})
        self.assertEqual(added, 2)
        cmds = [h["command"] for ev in ("UserPromptSubmit", "SessionEnd")
                for g in out["hooks"][ev] for h in g["hooks"]]
        self.assertIn(RECALL_CMD, cmds)
        self.assertIn(CAPTURE_CMD, cmds)

    def test_idempotent_no_duplicates(self):
        once, _ = merge_hooks({})
        twice, added = merge_hooks(once)
        self.assertEqual(added, 0)
        self.assertEqual(once, twice)

    def test_preserves_unrelated_hooks(self):
        existing = {"hooks": {"UserPromptSubmit": [
            {"hooks": [{"type": "command", "command": "my-own-hook"}]}]}}
        out, added = merge_hooks(existing)
        self.assertEqual(added, 2)  # recall added alongside, capture added
        ups = [h["command"] for g in out["hooks"]["UserPromptSubmit"]
               for h in g["hooks"]]
        self.assertIn("my-own-hook", ups)   # kept
        self.assertIn(RECALL_CMD, ups)      # added

    def test_capture_env_prefixes_command(self):
        out, added = merge_hooks({}, {"AEGIS_EXTRACT_MODE": "claude-code"})
        self.assertEqual(added, 2)
        cap = [h["command"] for g in out["hooks"]["SessionEnd"] for h in g["hooks"]][0]
        self.assertTrue(cap.startswith("AEGIS_EXTRACT_MODE=claude-code "))
        self.assertTrue(cap.endswith(CAPTURE_CMD))
        # recall is untouched by capture env
        rec = [h["command"] for g in out["hooks"]["UserPromptSubmit"] for h in g["hooks"]][0]
        self.assertEqual(rec, RECALL_CMD)

    def test_toggle_extraction_updates_in_place_no_duplicate(self):
        # start with plain capture, then re-run with extraction on
        once, _ = merge_hooks({})
        twice, changed = merge_hooks(once, {"AEGIS_EXTRACT_MODE": "anthropic"})
        self.assertEqual(changed, 1)  # capture updated, recall unchanged
        se = [h["command"] for g in twice["hooks"]["SessionEnd"] for h in g["hooks"]]
        self.assertEqual(len(se), 1)  # not duplicated
        self.assertTrue(se[0].startswith("AEGIS_EXTRACT_MODE=anthropic "))
        # and idempotent at the new setting
        again, changed2 = merge_hooks(twice, {"AEGIS_EXTRACT_MODE": "anthropic"})
        self.assertEqual(changed2, 0)
        self.assertEqual(twice, again)

    def test_extraction_off_reverts_to_plain(self):
        on, _ = merge_hooks({}, {"AEGIS_EXTRACT_MODE": "claude-code"})
        off, changed = merge_hooks(on)  # capture_env=None -> plain command
        self.assertEqual(changed, 1)
        se = [h["command"] for g in off["hooks"]["SessionEnd"] for h in g["hooks"]]
        self.assertEqual(se, [CAPTURE_CMD])


class TestMainWritesFiles(unittest.TestCase):
    def test_writes_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as d:
            argv = ["--dir", d, "--host", "h", "--port", "9470", "--yes",
                    "--no-verify"]
            self.assertEqual(main(argv), 0)
            mcp = json.load(open(os.path.join(d, ".mcp.json")))
            self.assertEqual(mcp["mcpServers"]["memory"]["env"]["AEGIS_HOST"], "h")
            settings = json.load(open(os.path.join(d, ".claude", "settings.json")))
            self.assertEqual(len(settings["hooks"]["UserPromptSubmit"]), 1)

            # second run: no duplicate hooks, memory entry already present (rc 0
            # because the entry is identical -> "exists" is fine without --force)
            self.assertEqual(main(argv), 0)
            settings2 = json.load(open(os.path.join(d, ".claude", "settings.json")))
            self.assertEqual(len(settings2["hooks"]["UserPromptSubmit"]), 1)

    def test_conflicting_entry_needs_force(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, ".mcp.json"), "w") as fh:
                json.dump({"mcpServers": {"memory": {"command": "old"}}}, fh)
            rc = main(["--dir", d, "--host", "h", "--yes", "--no-verify"])
            self.assertEqual(rc, 1)  # refuses to clobber without --force
            rc = main(["--dir", d, "--host", "h", "--yes", "--no-verify", "--force"])
            self.assertEqual(rc, 0)

    def test_extract_mode_writes_prefixed_capture_hook(self):
        with tempfile.TemporaryDirectory() as d:
            rc = main(["--dir", d, "--host", "h", "--yes", "--no-verify",
                       "--extract-mode", "claude-code"])
            self.assertEqual(rc, 0)
            settings = json.load(open(os.path.join(d, ".claude", "settings.json")))
            cap = [h["command"] for g in settings["hooks"]["SessionEnd"]
                   for h in g["hooks"]][0]
            self.assertIn("AEGIS_EXTRACT_MODE=claude-code", cap)
            self.assertTrue(cap.endswith("aegisdb-capture-hook"))
            # extraction is a hook concern, not the MCP server's env
            mcp = json.load(open(os.path.join(d, ".mcp.json")))
            self.assertNotIn("AEGIS_EXTRACT_MODE", mcp["mcpServers"]["memory"]["env"])

    def test_print_writes_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            rc = main(["--dir", d, "--host", "h", "--yes", "--print"])
            self.assertEqual(rc, 0)
            self.assertFalse(os.path.exists(os.path.join(d, ".mcp.json")))


if __name__ == "__main__":
    unittest.main()


class TestProjectConfig(unittest.TestCase):
    def test_auth_token_is_never_written(self):
        doc = build_project_config(namespace="n", host="h", port=1)
        self.assertNotIn("auth_token", doc)

    def test_mode_is_always_stated_so_none_is_expressible(self):
        doc = build_project_config(namespace="n", host="h", port=1)
        self.assertEqual(doc, {"namespace": "n", "aegis_host": "h",
                               "aegis_port": 1, "embedding_mode": "none"})
        self.assertNotIn("embedding_dimensions", doc)

    def test_embedding_written_when_on(self):
        doc = build_project_config(namespace="n", host="h", port=1,
                                   embedding_mode="local", embedding_dim=384)
        self.assertEqual(doc["embedding_mode"], "local")
        self.assertEqual(doc["embedding_dimensions"], 384)

    def test_merge_preserves_hand_edited_keys(self):
        out, changed = merge_project_config(
            {"recall_top_k": 9, "aegis_port": 1},
            {"namespace": "n", "aegis_host": "h", "aegis_port": 2})
        self.assertEqual(out["recall_top_k"], 9)   # not ours, left alone
        self.assertEqual(out["aegis_port"], 2)     # ours, updated
        self.assertEqual(changed, 3)

    def test_merge_reports_no_change_when_identical(self):
        entry = {"namespace": "n", "aegis_host": "h", "aegis_port": 1}
        _, changed = merge_project_config(dict(entry), entry)
        self.assertEqual(changed, 0)


class TestGitignore(unittest.TestCase):
    def test_skipped_outside_a_checkout(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIn("not a git checkout", ensure_gitignore(d))
            self.assertFalse(os.path.exists(os.path.join(d, ".gitignore")))

    def test_added_then_idempotent(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, ".git"))
            self.assertEqual(ensure_gitignore(d), "added")
            self.assertEqual(ensure_gitignore(d), "already ignored")
            body = open(os.path.join(d, ".gitignore")).read()
            self.assertEqual(body.count(GITIGNORE_LINE), 1)

    def test_existing_content_is_preserved_and_newline_safe(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, ".git"))
            with open(os.path.join(d, ".gitignore"), "w") as fh:
                fh.write("build/")  # no trailing newline
            ensure_gitignore(d)
            lines = open(os.path.join(d, ".gitignore")).read().splitlines()
            self.assertIn("build/", lines)
            self.assertIn(GITIGNORE_LINE, lines)


class TestMainPinsNamespace(unittest.TestCase):
    """Pinning is a no-op for an existing project and a fix for a future one:
    the value written is exactly what the path already derived."""

    def _run(self, d, *extra):
        argv = ["--dir", d, "--host", "h", "--port", "9470", "--yes",
                "--no-verify", *extra]
        self.assertEqual(main(argv), 0)
        return json.load(open(os.path.join(d, PROJECT_DIR, CONFIG_BASENAME)))

    def test_writes_the_value_the_path_already_derived(self):
        with tempfile.TemporaryDirectory() as d:
            doc = self._run(d)
            self.assertEqual(doc["namespace"], derive_namespace(env={}, cwd=d))

    def test_explicit_namespace_is_used_verbatim(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self._run(d, "--namespace", "mine")["namespace"],
                             "mine")

    def test_a_namespaced_token_leaves_it_blank(self):
        with tempfile.TemporaryDirectory() as d:
            doc = self._run(d, "--auth-token", "tok")
            self.assertEqual(doc["namespace"], "")
            mcp = json.load(open(os.path.join(d, ".mcp.json")))
            self.assertNotIn("AEGIS_NAMESPACE", mcp["mcpServers"]["memory"]["env"])

    def test_mcp_and_the_file_agree(self):
        with tempfile.TemporaryDirectory() as d:
            doc = self._run(d)
            mcp = json.load(open(os.path.join(d, ".mcp.json")))
            self.assertEqual(mcp["mcpServers"]["memory"]["env"]["AEGIS_NAMESPACE"],
                             doc["namespace"])

    def test_a_hook_reading_the_file_gets_what_the_server_got(self):
        # The point of the whole change: no env, and still the right settings.
        with tempfile.TemporaryDirectory() as d:
            doc = self._run(d, "--embedding-mode", "local", "--embedding-dim", "384")
            cfg = load_config(env={}, cwd=d)
            self.assertEqual(cfg.namespace, doc["namespace"])
            self.assertEqual(cfg.embedding_mode, "local")
            self.assertEqual(cfg.embedding_dimensions, 384)

    def test_print_writes_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(main(["--dir", d, "--yes", "--no-verify", "--print"]), 0)
            self.assertFalse(os.path.exists(os.path.join(d, PROJECT_DIR)))


class TestExplicitDirBeatsTheSessionProject(unittest.TestCase):
    """The high-severity bug this suite missed the first time.

    `project_root` prefers CLAUDE_PROJECT_DIR over the cwd it is passed — right
    for a hook, wrong for a CLI that was handed a directory. Pinning is what
    made it permanent: the wrong namespace was written into the other project's
    config, so it read and wrote the session project's memories from then on.
    """

    def setUp(self):
        self._saved = os.environ.get("CLAUDE_PROJECT_DIR")

    def tearDown(self):
        if self._saved is None:
            os.environ.pop("CLAUDE_PROJECT_DIR", None)
        else:
            os.environ["CLAUDE_PROJECT_DIR"] = self._saved

    def _pinned(self, d):
        return json.load(open(os.path.join(d, PROJECT_DIR, CONFIG_BASENAME)))["namespace"]

    def test_dir_wins_over_claude_project_dir(self):
        with tempfile.TemporaryDirectory() as session, \
             tempfile.TemporaryDirectory() as other:
            os.environ["CLAUDE_PROJECT_DIR"] = session
            main(["--dir", other, "--host", "h", "--port", "9470", "--yes",
                  "--no-verify"])
            self.assertEqual(self._pinned(other),
                             derive_namespace(env={}, cwd=other))
            self.assertNotEqual(self._pinned(other),
                                derive_namespace(env={}, cwd=session))

    def test_no_dir_uses_the_session_project_not_the_cwd(self):
        # A hook-shaped default: run from anywhere, scaffold the project root.
        with tempfile.TemporaryDirectory() as session:
            os.environ["CLAUDE_PROJECT_DIR"] = session
            main(["--host", "h", "--port", "9470", "--yes", "--no-verify"])
            self.assertTrue(os.path.isfile(
                os.path.join(session, PROJECT_DIR, CONFIG_BASENAME)))
            self.assertEqual(self._pinned(session),
                             derive_namespace(env={}, cwd=session))


class TestOwnedKeysAreRemovable(unittest.TestCase):
    def test_turning_embeddings_off_clears_the_stale_mode(self):
        out, changed = merge_project_config(
            {"embedding_mode": "local", "embedding_dimensions": 384,
             "recall_top_k": 9},
            build_project_config(namespace="n", host="h", port=1))
        self.assertEqual(out["embedding_mode"], "none")
        self.assertNotIn("embedding_dimensions", out)   # removed, not left stale
        self.assertEqual(out["recall_top_k"], 9)        # not ours, untouched
        self.assertTrue(changed)

    def test_init_can_actually_turn_embeddings_off(self):
        with tempfile.TemporaryDirectory() as d:
            argv = ["--dir", d, "--host", "h", "--port", "9470", "--yes",
                    "--no-verify"]
            main(argv + ["--embedding-mode", "local", "--embedding-dim", "384"])
            main(argv + ["--embedding-mode", "none", "--force"])
            cfg = load_config(env={}, cwd=d)
            self.assertEqual(cfg.embedding_mode, "none")


class TestGitDetection(unittest.TestCase):
    def test_a_worktree_dot_git_file_counts(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, ".git"), "w") as fh:
                fh.write("gitdir: /elsewhere/.git/worktrees/w\n")
            self.assertEqual(ensure_gitignore(d), "added")

    def test_a_subdirectory_of_a_checkout_counts(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, ".git"))
            sub = os.path.join(d, "services", "api")
            os.makedirs(sub)
            self.assertEqual(ensure_gitignore(sub), "added")
            # written beside the project, so the relative pattern still matches
            self.assertTrue(os.path.isfile(os.path.join(sub, ".gitignore")))
