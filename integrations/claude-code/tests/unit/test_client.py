"""Unit tests for the AegisDB NDJSON client (T011).

Uses an in-process fake TCP server so no real backend is needed: it validates
request framing, response parsing, and timeout -> AegisUnavailable behaviour.
"""
import json
import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.client import AegisClient, AegisUnavailable


class FakeServer:
    """One-shot-per-connection NDJSON server. `delay` simulates a slow backend;
    `reply` is the canned response dict (echoes the request if None)."""

    def __init__(self, reply=None, delay=0.0):
        self.reply = reply
        self.delay = delay
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.requests = []
        self._stop = False
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self._stop = True
        try:
            self.sock.close()
        except OSError:
            pass

    def _serve(self):
        while not self._stop:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            with conn:
                buf = b""
                while not buf.endswith(b"\n"):
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    buf += chunk
                if buf:
                    try:
                        self.requests.append(json.loads(buf))
                    except ValueError:
                        pass
                if self.delay:
                    time.sleep(self.delay)
                reply = self.reply if self.reply is not None else {"ok": True}
                try:
                    conn.sendall((json.dumps(reply) + "\n").encode())
                except OSError:
                    pass


class TestClient(unittest.TestCase):
    def test_request_roundtrip_and_framing(self):
        with FakeServer(reply={"ok": True, "record": {"id": 7}}) as srv:
            c = AegisClient("127.0.0.1", srv.port)
            resp = c.request({"operation": "insert", "data": "x"})
            self.assertTrue(resp["ok"])
            self.assertEqual(resp["record"]["id"], 7)
            time.sleep(0.05)
            self.assertEqual(srv.requests[0]["operation"], "insert")

    def test_auth_token_attached_when_set(self):
        with FakeServer(reply={"ok": True}) as srv:
            c = AegisClient("127.0.0.1", srv.port, auth_token="s3cret")
            c.request({"operation": "insert", "data": "x"})
            time.sleep(0.05)
            self.assertEqual(srv.requests[0].get("token"), "s3cret")

    def test_no_token_attached_when_unset(self):
        with FakeServer(reply={"ok": True}) as srv:
            c = AegisClient("127.0.0.1", srv.port)
            c.request({"operation": "insert", "data": "x"})
            time.sleep(0.05)
            self.assertNotIn("token", srv.requests[0])

    def test_ping_and_available(self):
        with FakeServer(reply={"ok": True, "version": "0.1.0", "phase": 4}) as srv:
            c = AegisClient("127.0.0.1", srv.port)
            self.assertTrue(c.available())
            self.assertEqual(c.ping()["version"], "0.1.0")

    def test_connection_refused_is_unavailable(self):
        # Nothing listening on this port.
        c = AegisClient("127.0.0.1", 1, connect_timeout_ms=200)
        self.assertFalse(c.available())
        with self.assertRaises(AegisUnavailable):
            c.request({"operation": "ping"})

    def test_read_timeout_is_unavailable(self):
        with FakeServer(reply={"ok": True}, delay=0.6) as srv:
            c = AegisClient("127.0.0.1", srv.port, read_timeout_ms=150)
            start = time.monotonic()
            with self.assertRaises(AegisUnavailable):
                c.request({"operation": "ping"})
            # Should give up around the timeout, well before the server replies.
            self.assertLess(time.monotonic() - start, 0.5)

    def test_per_request_timeout_override(self):
        with FakeServer(reply={"ok": True}, delay=0.3) as srv:
            c = AegisClient("127.0.0.1", srv.port, read_timeout_ms=2000)
            with self.assertRaises(AegisUnavailable):
                c.request({"operation": "ping"}, read_timeout_ms=100)


if __name__ == "__main__":
    unittest.main()