"""Unit tests for `aegisdb-doctor`.

Each check exists to turn one silent failure into a sentence, so what is pinned
here is the *distinctions*: a server that is down against one that is old, a
provider that is configured against one that is usable, a hook that is absent
against a settings file that will not parse. A doctor that reports the wrong
reason is worse than one that reports nothing, because it sends someone to fix
something that is not broken.
"""
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.config import CONFIG_BASENAME, PROJECT_DIR, Config  # noqa: E402
from aegis_mcp.doctor import (FAIL, OK, SKIP, WARN, Report,  # noqa: E402
                              check_config, check_dimension, check_embeddings,
                              check_hook_runs, check_hooks, check_read_path,
                              check_registration, main)
from aegis_mcp.extract import ExtractionProvider  # noqa: E402


def status(rep, name):
    return next(c["status"] for c in rep.checks if c["check"] == name)


def detail(rep, name):
    c = next(c for c in rep.checks if c["check"] == name)
    return c["detail"] + " " + c.get("fix", "")


class _Tools:
    """Stands in for MemoryTools where a check only needs its config."""

    def __init__(self, config):
        self.config = config


def project(**files):
    d = tempfile.mkdtemp()
    for rel, body in files.items():
        path = os.path.join(d, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(body if isinstance(body, str) else json.dumps(body))
    return d


class TestConfigCheck(unittest.TestCase):
    def test_a_missing_config_is_a_warning_naming_the_fix(self):
        rep = Report()
        cfg = check_config(rep, {}, project())
        self.assertEqual(status(rep, "config"), WARN)
        self.assertIn("aegisdb-init", detail(rep, "config"))
        self.assertIsNotNone(cfg)  # the rest of the run is still meaningful

    def test_a_discovered_file_says_so(self):
        d = project(**{f"{PROJECT_DIR}/{CONFIG_BASENAME}": {"aegis_port": 1}})
        rep = Report()
        check_config(rep, {}, d)
        self.assertEqual(status(rep, "config"), OK)
        self.assertIn("discovered", detail(rep, "config"))

    def test_a_named_file_says_who_named_it(self):
        """Which file is in force is the first thing to establish: a stale
        AEGIS_CONFIG explains every other setting being wrong."""
        d = project(**{"elsewhere.json": {"aegis_port": 1}})
        rep = Report()
        check_config(rep, {"AEGIS_CONFIG": os.path.join(d, "elsewhere.json")}, d)
        self.assertIn("AEGIS_CONFIG", detail(rep, "config"))

    def test_a_broken_config_stops_the_run(self):
        """Every later check would be reporting on settings nobody chose."""
        d = project(**{f"{PROJECT_DIR}/{CONFIG_BASENAME}": "{not json"})
        rep = Report()
        self.assertIsNone(check_config(rep, {}, d))
        self.assertEqual(status(rep, "config"), FAIL)


class TestDimensionCheck(unittest.TestCase):
    def test_agreement(self):
        rep = Report()
        check_dimension(rep, Config(embedding_mode="voyage",
                                    embedding_dimensions=1024),
                        {"embedding_dimensions": 1024})
        self.assertEqual(status(rep, "dimension"), OK)

    def test_a_mismatch_names_both_numbers_and_both_ways_out(self):
        rep = Report()
        check_dimension(rep, Config(embedding_mode="voyage",
                                    embedding_dimensions=384),
                        {"embedding_dimensions": 1024})
        self.assertEqual(status(rep, "dimension"), FAIL)
        said = detail(rep, "dimension")
        self.assertIn("384", said)
        self.assertIn("1024", said)
        self.assertIn("--embedding-dim", said)

    def test_an_old_server_is_a_warning_not_a_verdict(self):
        """It cannot be checked — which is not the same as being wrong."""
        rep = Report()
        check_dimension(rep, Config(embedding_mode="voyage"), {"ok": True})
        self.assertEqual(status(rep, "dimension"), WARN)

    def test_a_server_that_is_down_is_not_reported_as_old(self):
        """The distinction that sends someone to upgrade a server that is
        simply not running."""
        rep = Report()
        check_dimension(rep, Config(embedding_mode="voyage"), None)
        self.assertEqual(status(rep, "dimension"), SKIP)
        self.assertIn("unreachable", detail(rep, "dimension"))

    def test_embeddings_off_skips(self):
        rep = Report()
        check_dimension(rep, Config(), {"embedding_dimensions": 1024})
        self.assertEqual(status(rep, "dimension"), SKIP)


class TestEmbeddingsCheck(unittest.TestCase):
    def test_mode_none_is_a_warning_about_what_recall_becomes(self):
        rep = Report()
        check_embeddings(rep, Config())
        self.assertEqual(status(rep, "embeddings"), WARN)

    def test_a_configured_but_unusable_provider_fails_with_both_reasons(self):
        """A missing package and a missing key are independent, and naming one
        sends people to check what is already fine."""
        rep = Report()
        check_embeddings(rep, Config(embedding_mode="voyage"))
        self.assertEqual(status(rep, "embeddings"), FAIL)
        said = detail(rep, "embeddings")
        self.assertIn("pip install", said)
        self.assertIn("VOYAGE_API_KEY", said)


class TestWiringChecks(unittest.TestCase):
    def test_a_missing_mcp_file_and_a_broken_one_read_differently(self):
        rep = Report()
        check_registration(rep, project())
        self.assertIn("not registered", detail(rep, "mcp entry"))

        rep = Report()
        check_registration(rep, project(**{".mcp.json": "{oops"}))
        self.assertIn("not valid JSON", detail(rep, "mcp entry"))

    def test_other_mcp_servers_do_not_count(self):
        rep = Report()
        check_registration(rep, project(**{".mcp.json": {
            "mcpServers": {"github": {"command": "x"}}}}))
        self.assertEqual(status(rep, "mcp entry"), FAIL)
        self.assertIn("github", detail(rep, "mcp entry"))

    def test_registered(self):
        rep = Report()
        check_registration(rep, project(**{".mcp.json": {
            "mcpServers": {"memory": {"command": "uvx"}}}}))
        self.assertEqual(status(rep, "mcp entry"), OK)

    def test_each_missing_hook_is_named(self):
        """The quietest failure in the integration: nothing is injected,
        nothing is stored, and nothing anywhere says so."""
        rep = Report()
        check_hooks(rep, project(**{".claude/settings.json": {"hooks": {
            "UserPromptSubmit": [{"hooks": [
                {"command": "uvx --from aegisdb-mcp aegisdb-recall-hook"}]}]}}}))
        self.assertEqual(status(rep, "hooks"), FAIL)
        said = detail(rep, "hooks")
        self.assertIn("capture", said)
        self.assertNotIn("recall (", said)

    def test_both_hooks_wired(self):
        rep = Report()
        check_hooks(rep, project(**{".claude/settings.json": {"hooks": {
            "UserPromptSubmit": [{"hooks": [
                {"command": "uvx --from aegisdb-mcp aegisdb-recall-hook"}]}],
            "SessionEnd": [{"hooks": [
                {"command": "K=v uvx --from aegisdb-mcp aegisdb-capture-hook"}]}],
        }}}))
        self.assertEqual(status(rep, "hooks"), OK)


class _Usable(ExtractionProvider):
    def available(self):
        return True


class TestReadPathCheck(unittest.TestCase):
    def test_off_skips(self):
        rep = Report()
        check_read_path(rep, Config(), _Tools(Config()), True)
        self.assertEqual(status(rep, "read path"), SKIP)

    def test_no_backend_is_the_failure_that_needs_no_server(self):
        rep = Report()
        cfg = Config(ask_pattern=True)
        check_read_path(rep, cfg, _Tools(cfg), False)
        self.assertEqual(status(rep, "read path"), FAIL)
        self.assertIn("AEGIS_EXTRACT_MODE", detail(rep, "read path"))

    def test_offline_does_not_blame_the_vocabulary(self):
        """With no registry configured the vocabulary comes from the server, so
        while it is down there is nothing to say — and "no vocabulary" would
        read as a second, separate fault."""
        rep = Report()
        cfg = Config(ask_pattern=True, extract_mode="fake")
        check_read_path(rep, cfg, _Tools(cfg), False)
        self.assertEqual(status(rep, "read path"), WARN)
        self.assertIn("unchecked", detail(rep, "read path"))


class TestExitCode(unittest.TestCase):
    """Pinned against a port nothing answers on, and with stdout captured.

    A unit test that reached the default 9470 would talk to whatever AegisDB
    the developer happens to be running — passing or failing on the state of a
    machine, and pointing a diagnostic at somebody's real memory store.
    """

    def _run(self, argv, cwd=None):
        buf = io.StringIO()
        old = os.getcwd()
        os.chdir(cwd or project())
        try:
            with mock.patch.dict(os.environ, {"AEGIS_PORT": "9",
                                              "AEGIS_CONFIG": ""}, clear=False):
                with contextlib.redirect_stdout(buf):
                    code = main(argv)
        finally:
            os.chdir(old)
        return code, buf.getvalue()

    def test_a_broken_project_exits_nonzero(self):
        """So a pre-commit hook or a CI step can use it."""
        code, out = self._run(["--no-write"])
        self.assertEqual(code, 1)
        self.assertIn("aegisdb-init", out)

    def test_json_is_machine_readable(self):
        code, out = self._run(["--no-write", "--json"])
        doc = json.loads(out)
        self.assertFalse(doc["ok"])
        self.assertEqual(code, 1)
        self.assertTrue(all({"check", "status"} <= set(c) for c in doc["checks"]))

    def test_an_explicit_dir_reports_on_that_dir(self):
        """Run inside a Claude Code session, CLAUDE_PROJECT_DIR would otherwise
        point every check at the session's project instead."""
        target = project()
        with mock.patch.dict(os.environ, {"CLAUDE_PROJECT_DIR": "/nonexistent"},
                             clear=False):
            _, out = self._run(["--no-write", "--json", "--dir", target])
        self.assertIn(os.path.basename(target),
                      json.loads(out)["checks"][1]["detail"])


if __name__ == "__main__":
    unittest.main()


class TestHookActuallyRuns(unittest.TestCase):
    """Wired and working are different things.

    A hook present in `settings.json` whose command cannot run fails inside
    Claude Code with no message anywhere — and every file-based check passes,
    which is how the wiring can look complete while nothing is ever recalled.
    Only recall is executed: it reads, while capture writes, and a diagnostic
    must not store a memory to prove that storing works.
    """

    def run_cmd(self, cmd):
        rep = Report()
        check_hook_runs(rep, tempfile.mkdtemp(), cmd, timeout_s=20)
        return rep

    def test_a_hook_that_runs_passes(self):
        self.assertEqual(status(self.run_cmd("cat >/dev/null"), "hook runs"), OK)

    def test_a_silent_failure_is_named_as_a_launcher_problem(self):
        """Exit 1 with nothing on either stream: the command never started, and
        there is no other evidence to go on."""
        rep = self.run_cmd("sh -c 'exit 1'")
        self.assertEqual(status(rep, "hook runs"), FAIL)
        said = detail(rep, "hook runs")
        self.assertIn("printed nothing", said)
        self.assertIn("by hand", said)

    def test_a_loud_failure_relays_what_it_said(self):
        rep = self.run_cmd("sh -c 'echo ModuleNotFoundError >&2; exit 2'")
        self.assertEqual(status(rep, "hook runs"), FAIL)
        self.assertIn("ModuleNotFoundError", detail(rep, "hook runs"))
        self.assertIn("exits 2", detail(rep, "hook runs"))

    def test_a_hang_is_a_warning_naming_the_turn_budget(self):
        """A first `uvx` fetch is legitimately slow; a hook that always hangs is
        eating the time budget of every turn."""
        rep = Report()
        check_hook_runs(rep, tempfile.mkdtemp(), "sleep 5", timeout_s=0.5)
        self.assertEqual(status(rep, "hook runs"), WARN)
        self.assertIn("time budget", detail(rep, "hook runs"))

    def test_nothing_to_run_skips(self):
        rep = Report()
        check_hook_runs(rep, tempfile.mkdtemp(), None)
        self.assertEqual(status(rep, "hook runs"), SKIP)

    def test_the_hook_is_told_where_the_project_is(self):
        """It runs outside a session, so `CLAUDE_PROJECT_DIR` has to be set or
        the hook resolves config against the wrong directory."""
        root = tempfile.mkdtemp()
        rep = Report()
        out = os.path.join(root, "seen")
        check_hook_runs(rep, root, f"printf '%s' \"$CLAUDE_PROJECT_DIR\" > {out}",
                        timeout_s=20)
        with open(out) as fh:
            self.assertEqual(fh.read(), root)
