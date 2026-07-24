"""Unit tests for the aegisdb-init scaffolder: config building, non-destructive
merges, and idempotent file writes."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.init import (build_mcp_config, main, merge_hooks, merge_mcp,
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
