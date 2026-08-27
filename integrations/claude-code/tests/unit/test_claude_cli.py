"""Unit tests for the one place that spawns `claude` (aegis_mcp/claude_cli.py).

The invariant under test is not cosmetic. Both backends that shell out here run
inside a capture, and a `claude` session started without these guards ends,
fires SessionEnd, captures, and shells out again — with the worker detached from
the hook's process group, so the tree grows unreaped until the machine gives up.
"""
import os
import subprocess
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp import claude_cli  # noqa: E402
from aegis_mcp.claude_cli import CAPTURE_ACTIVE_ENV, child_env, headless_cmd  # noqa: E402
from aegis_mcp.extract import ClaudeCodeExtractionProvider  # noqa: E402
from aegis_mcp.summary import ClaudeCodeSummaryProvider  # noqa: E402


class _ProbeReset(unittest.TestCase):
    """`supports_safe_mode` caches for the life of the process."""

    def setUp(self):
        claude_cli._safe_mode_supported = None
        self.addCleanup(setattr, claude_cli, "_safe_mode_supported", None)


class TestHeadlessCommand(_ProbeReset):
    def test_it_disables_hooks_in_the_child(self):
        claude_cli._safe_mode_supported = True
        self.assertIn("--safe-mode", headless_cmd("why?"))

    def test_the_model_is_still_selectable(self):
        claude_cli._safe_mode_supported = True
        self.assertEqual(headless_cmd("why?", "haiku"),
                         ["claude", "-p", "why?", "--model", "haiku", "--safe-mode"])

    def test_an_older_cli_gets_no_flag_it_would_reject(self):
        """The mark alone stops the recursion; a bad flag would fail every call."""
        claude_cli._safe_mode_supported = False
        self.assertEqual(headless_cmd("why?"), ["claude", "-p", "why?"])

    def _probe(self, **help_result):
        with mock.patch("aegis_mcp.claude_cli.subprocess.run") as run:
            run.return_value = mock.Mock(**help_result)
            return claude_cli.supports_safe_mode(), run

    def test_support_is_probed_once_then_cached(self):
        with mock.patch("aegis_mcp.claude_cli.subprocess.run") as run:
            run.return_value = mock.Mock(returncode=0, stderr="",
                                         stdout="  --safe-mode  Start with ...")
            self.assertTrue(claude_cli.supports_safe_mode())
            self.assertTrue(claude_cli.supports_safe_mode())
        self.assertEqual(run.call_count, 1)

    def test_help_on_stderr_is_still_help(self):
        """A wrapper that prints usage to stderr must not cost us the flag."""
        found, _ = self._probe(returncode=0, stdout="",
                               stderr="  --safe-mode  Start with ...")
        self.assertTrue(found)

    def test_a_failing_help_is_not_mined_for_flags(self):
        """Non-zero means we do not know what this CLI accepts, so assume less."""
        found, _ = self._probe(returncode=1, stderr="",
                               stdout="  --safe-mode  Start with ...")
        self.assertFalse(found)

    def test_an_unprobeable_cli_reads_as_unsupported(self):
        with mock.patch("aegis_mcp.claude_cli.subprocess.run", side_effect=OSError):
            self.assertFalse(claude_cli.supports_safe_mode())

    def test_a_hanging_help_does_not_hang_the_capture(self):
        with mock.patch("aegis_mcp.claude_cli.subprocess.run",
                        side_effect=subprocess.TimeoutExpired("claude", 10)):
            self.assertFalse(claude_cli.supports_safe_mode())


class TestChildEnvironment(_ProbeReset):
    def test_the_child_is_told_a_capture_is_already_running(self):
        self.assertEqual(child_env()[CAPTURE_ACTIVE_ENV], "1")

    def test_the_rest_of_the_environment_survives(self):
        with mock.patch.dict(os.environ, {"PATH": "/nowhere"}):
            self.assertEqual(child_env()["PATH"], "/nowhere")


class TestProvidersUseIt(_ProbeReset):
    """Neither backend may assemble its own `claude` invocation."""

    def _run_and_capture_call(self, module, call):
        claude_cli._safe_mode_supported = True
        with mock.patch(f"aegis_mcp.{module}.subprocess.run") as run:
            run.return_value = mock.Mock(returncode=0, stdout="[]")
            call()
        self.assertEqual(run.call_count, 1)
        return run.call_args

    def test_extraction_marks_and_disables_hooks(self):
        args, kwargs = self._run_and_capture_call(
            "extract", lambda: ClaudeCodeExtractionProvider()._complete("p"))
        self.assertIn("--safe-mode", args[0])
        self.assertEqual(kwargs["env"][CAPTURE_ACTIVE_ENV], "1")

    def test_distillation_marks_and_disables_hooks(self):
        args, kwargs = self._run_and_capture_call(
            "summary", lambda: ClaudeCodeSummaryProvider().summarize(["a note"]))
        self.assertIn("--safe-mode", args[0])
        self.assertEqual(kwargs["env"][CAPTURE_ACTIVE_ENV], "1")


if __name__ == "__main__":
    unittest.main()
