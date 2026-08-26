"""`aegisdb-doctor` against a real server.

The unit tests script every answer, so what they cannot show is the thing the
command exists for: that a project wired the way `aegisdb-init` wires it comes
back clean, and that a dimension typed wrong — the mismatch nothing could detect
before `ping` reported the server's own — is caught before a single write is
refused.
"""
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from harness import PKG_ROOT, AegisServer, binary_available  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from aegis_mcp.client import AegisClient  # noqa: E402
from aegis_mcp.config import CONFIG_BASENAME, PROJECT_DIR  # noqa: E402
from aegis_mcp.doctor import main  # noqa: E402

# The path-run wiring rather than the `uvx` console script: it is one of the two
# documented forms, and it is the one that can actually run here. A fixture
# wiring `uvx` describes a project whose hooks do not work on a machine without
# it — which the `hook runs` check correctly fails, as CI demonstrated.
RECALL = f'python3 "{os.path.join(PKG_ROOT, "hooks", "recall_hook.py")}"'
CAPTURE = f'python3 "{os.path.join(PKG_ROOT, "hooks", "capture_hook.py")}"' 


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestDoctorE2E(unittest.TestCase):
    def _project(self, srv, **overrides):
        """A project wired the way `aegisdb-init` wires one."""
        d = tempfile.mkdtemp(prefix="aegis_doctor_")
        cfg = {"aegis_host": "127.0.0.1", "aegis_port": srv.port,
               "namespace": "doctor-e2e", "embedding_mode": "none"}
        cfg.update(overrides)
        os.makedirs(os.path.join(d, PROJECT_DIR))
        with open(os.path.join(d, PROJECT_DIR, CONFIG_BASENAME), "w") as fh:
            json.dump(cfg, fh)
        with open(os.path.join(d, ".mcp.json"), "w") as fh:
            json.dump({"mcpServers": {"memory": {"command": "uvx"}}}, fh)
        os.makedirs(os.path.join(d, ".claude"))
        with open(os.path.join(d, ".claude", "settings.json"), "w") as fh:
            json.dump({"hooks": {
                "UserPromptSubmit": [{"hooks": [{"command": RECALL}]}],
                "SessionEnd": [{"hooks": [{"command": CAPTURE}]}]}}, fh)
        return d

    def _run(self, project, *argv):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = main(["--json", "--dir", project, *argv])
        return code, json.loads(buf.getvalue())

    def _status(self, doc, name):
        return next(c["status"] for c in doc["checks"] if c["check"] == name)

    def test_a_hook_that_cannot_run_fails_a_wired_project(self):
        """The gap this closes end to end: `settings.json` names both hooks, so
        every file-based check passes, and the recall hook does nothing in a
        session. Before this the report was all green."""
        with AegisServer() as srv:
            d = self._project(srv)
            with open(os.path.join(d, ".claude", "settings.json"), "w") as fh:
                json.dump({"hooks": {
                    "UserPromptSubmit": [{"hooks": [
                        {"command": "sh -c 'exit 1'  # aegisdb-recall-hook"}]}],
                    "SessionEnd": [{"hooks": [{"command": CAPTURE}]}]}}, fh)
            code, doc = self._run(d)
            self.assertEqual(code, 1)
            self.assertEqual(self._status(doc, "hooks"), "ok")       # wired…
            self.assertEqual(self._status(doc, "hook runs"), "fail")  # …not working

    def test_a_wired_project_comes_back_clean(self):
        with AegisServer() as srv:
            code, doc = self._run(self._project(srv))
            self.assertTrue(doc["ok"], doc)
            self.assertEqual(code, 0)
            self.assertEqual(self._status(doc, "server"), "ok")
            self.assertEqual(self._status(doc, "round trip"), "ok")

    def test_the_round_trip_leaves_nothing_behind(self):
        """A diagnostic that accumulates records in the store it is diagnosing
        would be its own slow failure."""
        with AegisServer() as srv:
            project = self._project(srv)
            for _ in range(3):
                self.assertTrue(self._run(project)[1]["ok"])
            client = AegisClient("127.0.0.1", srv.port)
            found = client.request({"operation": "search",
                                    "tags": ["aegisdb-doctor"], "top_k": 10,
                                    "agent_id": "doctor-e2e"})
            self.assertEqual(found.get("records", []), [])

    def test_a_dimension_typed_wrong_is_caught_before_any_write(self):
        """The check `ping` made possible. The client's number is plausible and
        simply is not this server's."""
        with AegisServer() as srv:
            project = self._project(srv, embedding_mode="voyage",
                                    embedding_dimensions=srv.dim + 1)
            code, doc = self._run(project, "--no-write")
            self.assertEqual(code, 1)
            self.assertEqual(self._status(doc, "dimension"), "fail")
            said = next(c for c in doc["checks"] if c["check"] == "dimension")
            self.assertIn(str(srv.dim), said["detail"] + said.get("fix", ""))

    def test_an_agreed_dimension_passes_against_the_same_server(self):
        with AegisServer() as srv:
            project = self._project(srv, embedding_mode="voyage",
                                    embedding_dimensions=srv.dim)
            _, doc = self._run(project, "--no-write")
            self.assertEqual(self._status(doc, "dimension"), "ok")

    def test_a_server_that_is_down_fails_once(self):
        """One outage should read as one fault, not as a column of red."""
        with AegisServer() as srv:
            project = self._project(srv)
        code, doc = self._run(project)  # server stopped on the way out
        self.assertEqual(code, 1)
        fails = [c["check"] for c in doc["checks"] if c["status"] == "fail"]
        self.assertEqual(fails, ["server"])
        self.assertEqual(self._status(doc, "round trip"), "skip")


if __name__ == "__main__":
    unittest.main()
