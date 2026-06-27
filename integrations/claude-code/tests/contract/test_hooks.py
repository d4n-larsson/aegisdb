"""Contract tests for the recall and capture hook scripts (T026, T033).

Runs the hook scripts as subprocesses (as Claude Code would), feeding the hook
event on stdin and asserting the stdout/exit-code contract in contracts/hooks.md.
"""
import json
import os
import subprocess
import sys
import tempfile
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
    }
    env.update({k: str(v) for k, v in extra.items()})
    return env


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


if __name__ == "__main__":
    unittest.main()