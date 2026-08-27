"""Contract tests for the recall and capture hook scripts (T026, T033).

Runs the hook scripts as subprocesses (as Claude Code would), feeding the hook
event on stdin and asserting the stdout/exit-code contract in contracts/hooks.md.
"""
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from harness import AegisServer, binary_available, make_config, PKG_ROOT  # noqa: E402

RECALL_HOOK = os.path.join(PKG_ROOT, "hooks", "recall_hook.py")
CAPTURE_HOOK = os.path.join(PKG_ROOT, "hooks", "capture_hook.py")


def _run_hook(path, event, env):
    full_env = dict(os.environ)
    full_env.update(env)
    proc = subprocess.run([sys.executable, path], input=json.dumps(event).encode(),
                          capture_output=True, env=full_env, timeout=20)
    return proc


def _server_env(srv, namespace="hook-ns", mode="fake", **extra):
    env = {
        "AEGIS_HOST": "127.0.0.1", "AEGIS_PORT": str(srv.port),
        "AEGIS_NAMESPACE": namespace, "AEGIS_EMBEDDING_MODE": mode,
        "AEGIS_EMBEDDING_DIMENSIONS": str(srv.dim),
        # Capture detaches by default, so its exit code means "started". The
        # assertions below are about what got STORED, which needs the work
        # finished before the hook returns.
        "AEGIS_CAPTURE_DETACH": "0",
        # Pinned because a capture worker resolves config through
        # `CLAUDE_PROJECT_DIR` when it is set, which in a Claude Code session is
        # this repo -- whose own .aegisdb/config.json runs `extract_mode:
        # claude-code`. Unpinned, the suite shells out to a real model: minutes
        # of latency, a bill, and a test whose result depends on a checked-in
        # config it never mentions.
        "AEGIS_EXTRACT_MODE": "none", "AEGIS_SUMMARY_MODE": "none",
    }
    env.update({k: str(v) for k, v in extra.items()})
    return env


def _await_worker(stderr, timeout=10.0):
    """Block until the worker the hook announced has exited.

    The worker reads the transcript *after* the hook returns, so a test that
    deletes it on return is racing a process it just asserted exists (the same
    constraint `TestCaptureHookDetachedE2E` documents). It is not our child --
    it has its own session and was reparented -- so existence is probed with
    signal 0 rather than waited on. No announcement means no fork, and nothing
    to wait for.
    """
    m = re.search(rb"detached \(pid (\d+)\)", stderr or b"")
    if not m:
        return
    pid = int(m.group(1))
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            os.kill(pid, 0)
        except OSError:
            return
        time.sleep(0.05)


def _write_transcript(lines):
    fd, path = tempfile.mkstemp(suffix=".jsonl")
    with os.fdopen(fd, "w") as fh:
        for role, text in lines:
            fh.write(json.dumps({"type": role, "content": text}) + "\n")
    return path


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestRecallHookContract(unittest.TestCase):
    def _seed(self, srv, namespace):
        from aegis_mcp.client import AegisClient
        from aegis_mcp.embeddings import FakeProvider
        from aegis_mcp.tools import MemoryTools
        cfg = make_config(srv, namespace=namespace)
        MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port),
                    FakeProvider(srv.dim)).save(
            "Deploy the project by running make ship", tags=["ops"])

    def test_injects_additional_context(self):
        with AegisServer() as srv:
            self._seed(srv, "hook-ns")
            event = {"hook_event_name": "UserPromptSubmit",
                     "prompt": "how do I deploy the project?", "cwd": os.getcwd()}
            proc = _run_hook(RECALL_HOOK, event, _server_env(srv))
            self.assertEqual(proc.returncode, 0)
            out = json.loads(proc.stdout.decode())
            self.assertEqual(out["hookSpecificOutput"]["hookEventName"], "UserPromptSubmit")
            self.assertIn("make ship", out["hookSpecificOutput"]["additionalContext"])

    def test_injects_nothing_inside_a_capture(self):
        """A capture that shells out to a model starts a session, and that
        session's prompt does not want memories injected into it.

        Written as an A/B against `test_injects_additional_context` above --
        same server, same seeded memory, same prompt, only the mark differs --
        because the negative alone proves nothing: a recall hook that is simply
        broken prints nothing too, and so does one aimed at an empty namespace.
        The control has to inject before the refusal means anything.
        """
        with AegisServer() as srv:
            self._seed(srv, "hook-ns")
            event = {"hook_event_name": "UserPromptSubmit",
                     "prompt": "how do I deploy the project?", "cwd": os.getcwd()}
            env = _server_env(srv)
            control = _run_hook(RECALL_HOOK, event, env)
            self.assertIn("make ship", control.stdout.decode())

            marked = _run_hook(RECALL_HOOK, event,
                               {**env, "AEGIS_CAPTURE_ACTIVE": "1"})
            self.assertEqual(marked.returncode, 0)
            self.assertEqual(marked.stdout.decode().strip(), "")

    def test_no_match_empty_output_exit0(self):
        with AegisServer() as srv:
            event = {"hook_event_name": "UserPromptSubmit",
                     "prompt": "totally unrelated zebra question", "cwd": os.getcwd()}
            proc = _run_hook(RECALL_HOOK, event, _server_env(srv, namespace="empty-ns"))
            self.assertEqual(proc.returncode, 0)
            self.assertEqual(proc.stdout.decode().strip(), "")

    def test_backend_down_exit0_no_stdout(self):
        with AegisServer() as srv:
            env = _server_env(srv)
        # server stopped; hook must still exit 0 with no injected context
        event = {"hook_event_name": "UserPromptSubmit", "prompt": "deploy?",
                 "cwd": os.getcwd()}
        proc = _run_hook(RECALL_HOOK, event, {**env, "AEGIS_RECALL_TIME_BUDGET_MS": "300"})
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(proc.stdout.decode().strip(), "")


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestCaptureHookContract(unittest.TestCase):
    def test_salient_session_persists(self):
        with AegisServer() as srv:
            transcript = _write_transcript([
                ("assistant", "We decided to use tabs for indentation going forward."),
            ])
            try:
                event = {"hook_event_name": "SessionEnd", "cwd": os.getcwd(),
                         "transcript_path": transcript}
                proc = _run_hook(CAPTURE_HOOK, event, _server_env(srv, namespace="cap-ns"))
                self.assertEqual(proc.returncode, 0)
            finally:
                os.remove(transcript)
            # Verify a memory was actually stored in that namespace.
            from aegis_mcp.client import AegisClient
            from aegis_mcp.embeddings import FakeProvider
            from aegis_mcp.tools import MemoryTools
            cfg = make_config(srv, namespace="cap-ns")
            res = MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port),
                              FakeProvider(srv.dim)).search(query="indentation", top_k=5)
            self.assertTrue(any("tabs" in m["text"].lower() for m in res["memories"]))

    def test_nonsalient_session_persists_nothing(self):
        with AegisServer() as srv:
            transcript = _write_transcript([("user", "hi"), ("assistant", "hello")])
            try:
                event = {"hook_event_name": "SessionEnd", "cwd": os.getcwd(),
                         "transcript_path": transcript}
                proc = _run_hook(CAPTURE_HOOK, event, _server_env(srv, namespace="cap-empty"))
                self.assertEqual(proc.returncode, 0)
            finally:
                os.remove(transcript)
            from aegis_mcp.client import AegisClient
            from aegis_mcp.embeddings import FakeProvider
            from aegis_mcp.tools import MemoryTools
            cfg = make_config(srv, namespace="cap-empty")
            res = MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port),
                              FakeProvider(srv.dim)).search(start_time=0, end_time=9_999_999_999_999, top_k=50)
            self.assertEqual(res["total"], 0)

    def test_backend_down_exit0(self):
        with AegisServer() as srv:
            env = _server_env(srv)
        transcript = _write_transcript([("assistant", "We decided to use tabs going forward.")])
        try:
            event = {"hook_event_name": "SessionEnd", "cwd": os.getcwd(),
                     "transcript_path": transcript}
            proc = _run_hook(CAPTURE_HOOK, event, env)
            self.assertEqual(proc.returncode, 0)
        finally:
            os.remove(transcript)


# What Claude Code does to a SessionEnd hook that has not finished: it kills the
# hook's process group ~1.5s in ("failed: Hook cancelled"). Capture takes tens of
# seconds with `local` embeddings or an extraction backend, so surviving that is
# the difference between capturing a session and silently capturing nothing.
_DETACH_DRIVER = """
import os, sys, time
sys.path.insert(0, %r)
from aegis_mcp import hooks
if hooks._detach():
    sys.exit(0)          # the hook returns at once, as Claude Code requires
time.sleep(%s)           # still working when the cancellation lands
open(%r, "w").write("survived")
"""


class TestCaptureDetach(unittest.TestCase):
    def _driver(self, marker, sleep_s):
        return _DETACH_DRIVER % (PKG_ROOT, sleep_s, marker)

    def test_worker_survives_process_group_kill(self):
        marker = os.path.join(tempfile.mkdtemp(), "marker")
        env = dict(os.environ)
        env.pop("AEGIS_CAPTURE_DETACH", None)  # detached is the default
        proc = subprocess.Popen([sys.executable, "-c", self._driver(marker, 2)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                env=env, start_new_session=True)
        pgid = os.getpgid(proc.pid)                 # the group Claude Code kills
        self.assertEqual(proc.wait(timeout=10), 0)  # returns without waiting
        self.assertFalse(os.path.exists(marker))    # the work is still running
        try:
            os.killpg(pgid, signal.SIGKILL)  # "Hook cancelled"
        except ProcessLookupError:
            pass  # nothing left in the hook's group at all: the worker is out

        deadline = time.time() + 15
        while time.time() < deadline and not os.path.exists(marker):
            time.sleep(0.1)
        self.assertTrue(os.path.exists(marker),
                        "detached capture died with the hook's process group")

    def test_inline_when_disabled_dies_with_the_group(self):
        """The control: without detaching, the cancellation takes the work with it.

        Which is what shipped, and why nothing was ever captured.
        """
        marker = os.path.join(tempfile.mkdtemp(), "marker")
        env = dict(os.environ)
        env["AEGIS_CAPTURE_DETACH"] = "0"
        proc = subprocess.Popen([sys.executable, "-c", self._driver(marker, 5)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                env=env, start_new_session=True)
        time.sleep(1.0)
        self.assertIsNone(proc.poll(), "inline capture should still be working")
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)  # "Hook cancelled"
        proc.wait(timeout=5)
        time.sleep(1.0)
        self.assertFalse(os.path.exists(marker))


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestCaptureHookDetachedE2E(unittest.TestCase):
    def test_detached_capture_stores_after_the_hook_returns(self):
        with AegisServer() as srv:
            transcript = _write_transcript([
                ("assistant", "We decided to use spaces for indentation going forward."),
            ])
            env = _server_env(srv, namespace="cap-detached")
            env["AEGIS_CAPTURE_DETACH"] = "1"
            event = {"hook_event_name": "SessionEnd", "cwd": os.getcwd(),
                     "transcript_path": transcript}
            proc = _run_hook(CAPTURE_HOOK, event, env)
            self.assertEqual(proc.returncode, 0)
            self.assertIn(b"detached", proc.stderr)

            from aegis_mcp.client import AegisClient
            from aegis_mcp.embeddings import FakeProvider
            from aegis_mcp.tools import MemoryTools
            cfg = make_config(srv, namespace="cap-detached")
            tools = MemoryTools(cfg, AegisClient(cfg.aegis_host, cfg.aegis_port),
                                FakeProvider(srv.dim))
            deadline = time.time() + 20
            found = False
            while time.time() < deadline and not found:
                res = tools.search(query="indentation", top_k=5)
                found = any("spaces" in m["text"].lower()
                            for m in res.get("memories", []))
                if not found:
                    time.sleep(0.2)
            # The transcript has to outlive the hook: the worker reads it after
            # the hook returns, so a caller that deletes it on return loses the
            # capture. Claude Code keeps its transcripts, so this constrains
            # tests -- and anything else that cleans up eagerly.
            os.remove(transcript)
            self.assertTrue(found, "detached worker stored nothing")


# `extract_mode: claude-code` extracts by running `claude -p`, which is a session,
# whose end fires this hook again. Detached (above), each generation outlives the
# process group Claude Code kills, so the tree grows unreaped until the machine
# is out of processes -- observed as an IDE dying. Both hooks therefore refuse to
# run inside a capture; the recall half of that is asserted against a live server
# in TestRecallHookContract, where a control can prove the refusal is not just a
# hook that never injects anything. No server is needed for the capture half: the
# refusal comes before the config load and before the fork.
class TestHooksRefuseToRunInsideACapture(unittest.TestCase):
    def _capture(self, env):
        transcript = _write_transcript([
            ("assistant", "We decided to use tabs for indentation going forward."),
        ])
        event = {"hook_event_name": "SessionEnd", "cwd": os.getcwd(),
                 "transcript_path": transcript}
        # Every one of these is pinned rather than inherited, because the
        # ambient environment decides the very thing being measured:
        #
        #   CAPTURE_DETACH  the control asserts a fork happened. `0` is what the
        #                   README tells operators to export and what
        #                   `_server_env` sets, and under it nothing forks --
        #                   the control fails and the guard test passes vacuously.
        #   EXTRACT/SUMMARY a worker prefers CLAUDE_PROJECT_DIR over `cwd`, which
        #                   in a Claude Code session is this repo, whose
        #                   .aegisdb/config.json sets `extract_mode:
        #                   claude-code`. A test asserting captures do not spawn
        #                   `claude` must not itself spawn `claude`.
        #   HOST/PORT       an address nothing answers on, so the control's
        #                   worker gives up instead of reaching a real store.
        base = {"AEGIS_HOST": "127.0.0.1", "AEGIS_PORT": "1",
                "AEGIS_EMBEDDING_MODE": "fake", "AEGIS_NAMESPACE": "recursion-ns",
                "AEGIS_CAPTURE_DETACH": "1",
                "AEGIS_EXTRACT_MODE": "none", "AEGIS_SUMMARY_MODE": "none"}
        try:
            proc = _run_hook(CAPTURE_HOOK, event, {**base, **env})
            _await_worker(proc.stderr)  # it reads the transcript after we return
            return proc
        finally:
            if os.path.exists(transcript):
                os.remove(transcript)

    def test_a_marked_capture_starts_no_worker(self):
        proc = self._capture({"AEGIS_CAPTURE_ACTIVE": "1"})
        self.assertEqual(proc.returncode, 0)
        self.assertNotIn(b"detached", proc.stderr)  # nothing forked, nothing ran

    def test_the_control_still_captures(self):
        """Unmarked, the same session forks a worker -- the guard is not a mute."""
        proc = self._capture({})
        self.assertEqual(proc.returncode, 0)
        self.assertIn(b"detached", proc.stderr)

    def test_off_is_a_value_the_mark_can_hold(self):
        self.assertIn(b"detached", self._capture({"AEGIS_CAPTURE_ACTIVE": "0"}).stderr)


if __name__ == "__main__":
    unittest.main()
