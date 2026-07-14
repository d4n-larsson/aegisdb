"""Live end-to-end test: launch a real aegisdb, scrape it through the exporter.

Skips automatically when the aegisdb binary is not built (mirrors the
claude-code integration tests). Exercises the whole path — real ``stats`` over
TCP, rendering, and the ``/metrics`` HTTP endpoint — plus the down path.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.request
from http.server import ThreadingHTTPServer

_HERE = os.path.dirname(os.path.abspath(__file__))
PKG_ROOT = os.path.dirname(os.path.dirname(_HERE))            # prometheus-exporter
REPO_ROOT = os.path.dirname(os.path.dirname(PKG_ROOT))        # repo root
AEGIS_BIN = os.path.join(REPO_ROOT, "build", "aegisdb")
sys.path.insert(0, PKG_ROOT)

from aegis_exporter import (Config, fetch_stats, make_handler,  # noqa: E402
                            render, scrape_text)


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _send(port, payload):
    line = (json.dumps(payload) + "\n").encode()
    with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
        sock.sendall(line)
        buf = bytearray()
        while not buf.endswith(b"\n"):
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.decode())


@unittest.skipUnless(os.path.exists(AEGIS_BIN), "aegisdb binary not built")
class TestLive(unittest.TestCase):
    def setUp(self):
        self.port = _free_port()
        self.datadir = tempfile.mkdtemp(prefix="aegis_exp_")
        self.proc = subprocess.Popen(
            [AEGIS_BIN, "--data-dir", self.datadir, "--port", str(self.port),
             "--phase", "4", "--embedding-dim", "16"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    break
            except OSError:
                time.sleep(0.1)
        else:
            self.tearDown()
            self.fail("aegisdb did not start")
        self.cfg = Config(env={"AEGIS_HOST": "127.0.0.1",
                               "AEGIS_PORT": str(self.port)})

    def tearDown(self):
        if self.proc:
            self.proc.kill()
            self.proc.wait()

    def test_fetch_and_render(self):
        r = _send(self.port, {"operation": "insert", "type": "episodic",
                              "data": "hello"})
        self.assertTrue(r.get("ok"), r)
        stats = fetch_stats(self.cfg)
        self.assertTrue(stats.get("ok"))
        self.assertGreaterEqual(stats["records"], 1)
        text = render(stats, up=True, scrape_seconds=0.0)
        self.assertIn("aegisdb_up 1", text)
        self.assertIn("aegisdb_records", text)
        self.assertIn("# TYPE aegisdb_requests_total counter", text)

    def test_metrics_http_endpoint(self):
        exp_port = _free_port()
        httpd = ThreadingHTTPServer(("127.0.0.1", exp_port),
                                    make_handler(self.cfg))
        t = threading.Thread(target=httpd.serve_forever, daemon=True)
        t.start()
        try:
            with urllib.request.urlopen(
                    f"http://127.0.0.1:{exp_port}/metrics", timeout=3) as resp:
                self.assertEqual(resp.status, 200)
                self.assertIn("text/plain", resp.headers.get("Content-Type", ""))
                body = resp.read().decode()
            self.assertIn("aegisdb_up 1", body)
            self.assertIn("aegisdb_info{", body)
        finally:
            httpd.shutdown()
            httpd.server_close()

    def test_down_path_when_server_gone(self):
        # Point the exporter at a dead port: a scrape must still succeed and
        # report aegisdb_up 0 (never raise), so alerting works.
        dead = Config(env={"AEGIS_HOST": "127.0.0.1",
                           "AEGIS_PORT": str(_free_port()),
                           "AEGIS_CONNECT_TIMEOUT_MS": "200"})
        text = scrape_text(dead)
        self.assertIn("aegisdb_up 0", text)
        self.assertIn("aegisdb_scrape_error{", text)


if __name__ == "__main__":
    unittest.main()