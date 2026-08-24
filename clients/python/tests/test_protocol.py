"""The transport and the error translation, against a fake server.

No aegisdb binary needed: these pin the behaviour that is this client's own
rather than the server's — how a refusal becomes an exception, what a reused
connection does when it goes stale, and the rule that an unspecified argument
must not become a client-invented default.
"""
import json
import os
import socket
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from aegisdb import (AegisClient, AegisRequestError, AegisUnavailable,  # noqa: E402
                     Forbidden, MemoryLimit, NotFound, NotReady, RateLimited)
from aegisdb.client import _put  # noqa: E402


class FakeServer:
    """One-line-in, one-line-out, with scriptable behaviour per connection."""

    def __init__(self, handler):
        self.handler = handler
        self.requests = []
        self.connections = 0
        self._sock = socket.socket()
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(8)
        self.port = self._sock.getsockname()[1]
        self._stop = False
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self):
        while not self._stop:
            try:
                conn, _ = self._sock.accept()
            except OSError:
                return
            self.connections += 1
            threading.Thread(target=self._session, args=(conn,),
                             daemon=True).start()

    def _session(self, conn):
        n = self.connections
        with conn:
            buf = b""
            while True:
                try:
                    chunk = conn.recv(65536)
                except OSError:
                    return
                if not chunk:
                    return
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    req = json.loads(line)
                    self.requests.append(req)
                    reply = self.handler(req, n)
                    if reply is None:
                        return  # close without answering
                    # Raw bytes go out verbatim, which is how a malformed
                    # response gets tested at all.
                    raw = isinstance(reply, bytes)
                    out = reply if raw else (json.dumps(reply) + "\n").encode()
                    try:
                        conn.sendall(out)
                    except OSError:
                        return
                    if raw:
                        # Raw replies close afterwards, which is what a server
                        # dying mid-response looks like — the only way the
                        # truncated path is reachable without a timeout.
                        return

    def close(self):
        self._stop = True
        try:
            self._sock.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def ok(**fields):
    return lambda req, n: {"ok": True, **fields}


def refuse(code, message="no"):
    return lambda req, n: {"ok": False, "error": {"code": code,
                                                  "message": message}}


class TestErrorTranslation(unittest.TestCase):
    def test_each_code_maps_to_its_own_exception(self):
        for code, exc in (("NOT_FOUND", NotFound), ("FORBIDDEN", Forbidden),
                          ("NOT_READY", NotReady), ("RATE_LIMITED", RateLimited),
                          ("MEMORY_LIMIT", MemoryLimit)):
            with FakeServer(refuse(code, "because")) as srv:
                db = AegisClient(port=srv.port)
                with self.assertRaises(exc) as caught:
                    db.get(1)
                self.assertEqual(caught.exception.code, code)
                self.assertEqual(caught.exception.message, "because")
                db.close()

    def test_an_unknown_code_still_raises_something_catchable(self):
        """A server newer than this client must degrade to the base class, not
        to a KeyError from the lookup table."""
        with FakeServer(refuse("TEAPOT", "short and stout")) as srv:
            db = AegisClient(port=srv.port)
            with self.assertRaises(AegisRequestError) as caught:
                db.ping()
            self.assertEqual(caught.exception.code, "TEAPOT")
            self.assertNotIsInstance(caught.exception, NotFound)
            db.close()

    def test_a_refusal_with_no_error_object_is_reported_not_smoothed(self):
        with FakeServer(lambda req, n: {"ok": False}) as srv:
            db = AegisClient(port=srv.port)
            with self.assertRaises(AegisRequestError) as caught:
                db.ping()
            self.assertEqual(caught.exception.code, "INTERNAL")
            db.close()

    def test_raise_on_error_false_returns_the_envelope(self):
        with FakeServer(refuse("NOT_FOUND")) as srv:
            db = AegisClient(port=srv.port)
            resp = db.request({"operation": "get", "id": 1},
                              raise_on_error=False)
            self.assertFalse(resp["ok"])
            self.assertEqual(resp["error"]["code"], "NOT_FOUND")
            db.close()

    def test_unavailable_is_not_a_request_error(self):
        """The distinction the retry logic rests on: a refusal means the server
        did not act, while an unanswered request says nothing either way."""
        db = AegisClient(port=1, connect_timeout=0.2)
        with self.assertRaises(AegisUnavailable):
            db.ping()
        self.assertNotIsInstance(AegisUnavailable("x"), AegisRequestError)


class TestConnectionReuse(unittest.TestCase):
    def test_one_connection_serves_many_requests(self):
        with FakeServer(ok(pong=True)) as srv:
            with AegisClient(port=srv.port) as db:
                for _ in range(5):
                    db.ping()
            self.assertEqual(srv.connections, 1,
                             "reuse means one connection, not five")

    def test_reuse_off_opens_one_per_request(self):
        with FakeServer(ok()) as srv:
            with AegisClient(port=srv.port, reuse=False) as db:
                for _ in range(3):
                    db.ping()
            self.assertEqual(srv.connections, 3)

    def test_a_reaped_connection_is_retried_once(self):
        """What an idle-reaped connection looks like: the first request on the
        second connection gets no answer and the socket closes. A client that
        surfaced that would fail on any call after a pause longer than
        --idle-timeout-sec."""
        state = {"drop": False}

        def handler(req, conn_no):
            if conn_no == 1 and state["drop"]:
                return None  # close without answering, as a reaped one does
            state["drop"] = True
            return {"ok": True, "n": conn_no}

        with FakeServer(handler) as srv:
            with AegisClient(port=srv.port) as db:
                self.assertEqual(db.ping()["n"], 1)
                # The reused connection is now dead; this must still answer.
                self.assertEqual(db.ping()["n"], 2)
            self.assertEqual(srv.connections, 2)

    def test_retry_stale_off_surfaces_the_failure(self):
        state = {"drop": False}

        def handler(req, conn_no):
            if state["drop"]:
                return None
            state["drop"] = True
            return {"ok": True}

        with FakeServer(handler) as srv:
            with AegisClient(port=srv.port, retry_stale=False) as db:
                db.ping()
                with self.assertRaises(AegisUnavailable):
                    db.ping()

    def test_a_fresh_connection_failure_is_not_retried(self):
        """The retry exists for a *stale* connection. A server that refuses to
        answer a brand-new one is broken, and hammering it twice per call would
        double the load at the worst moment."""
        with FakeServer(lambda req, n: None) as srv:
            with AegisClient(port=srv.port) as db:
                with self.assertRaises(AegisUnavailable):
                    db.ping()
            self.assertEqual(srv.connections, 1)

    def test_a_malformed_response_is_unavailable_not_a_crash(self):
        """A complete line that is not JSON. Unavailable rather than a
        JSONDecodeError escaping the package: a caller catching AegisError
        should not also have to catch the parser's."""
        with FakeServer(lambda req, n: b"not json at all\n") as srv:
            with AegisClient(port=srv.port) as db:
                with self.assertRaises(AegisUnavailable) as caught:
                    db.ping()
                self.assertIn("malformed", str(caught.exception))

    def test_a_response_cut_mid_line_is_unavailable(self):
        """No trailing newline means the response is incomplete. Parsing what
        arrived would risk acting on a truncated record."""
        with FakeServer(lambda req, n: b'{"ok": tr') as srv:
            with AegisClient(port=srv.port, reuse=False,
                             read_timeout=2.0) as db:
                with self.assertRaises(AegisUnavailable) as caught:
                    db.ping()
                self.assertIn("truncated", str(caught.exception))


class TestPayloadShape(unittest.TestCase):
    def test_token_and_agent_id_are_defaults_not_overrides(self):
        with FakeServer(ok()) as srv:
            with AegisClient(port=srv.port, token="tok",
                             agent_id="mine") as db:
                db.get(1)
                db.get(2, agent_id="theirs")
            first, second = srv.requests
            self.assertEqual(first["token"], "tok")
            self.assertEqual(first["agent_id"], "mine")
            self.assertEqual(second["agent_id"], "theirs",
                             "an explicit argument wins over the default")

    def test_no_token_or_agent_id_sends_neither(self):
        with FakeServer(ok()) as srv:
            with AegisClient(port=srv.port) as db:
                db.ping()
            self.assertNotIn("token", srv.requests[0])
            self.assertNotIn("agent_id", srv.requests[0])

    def test_the_callers_dict_is_not_mutated(self):
        """`request` adds the token and namespace, and doing that in place
        would rewrite a payload the caller may reuse or log."""
        with FakeServer(ok()) as srv:
            with AegisClient(port=srv.port, token="tok") as db:
                payload = {"operation": "ping"}
                db.request(payload)
                self.assertEqual(payload, {"operation": "ping"})

    def test_unspecified_arguments_are_omitted_entirely(self):
        """None means "not specified", so the *server's* default applies. A
        client-side default would drift from it, and silently win."""
        with FakeServer(ok()) as srv:
            with AegisClient(port=srv.port) as db:
                db.search(query="x")
            sent = srv.requests[0]
            self.assertEqual(set(sent), {"operation", "query"})

    def test_falsy_values_are_not_treated_as_absent(self):
        """The trap in an `if value:` filter. `limit=0` is the conflicts probe,
        `top_k=0` and `subsume=False` are meaningful, and dropping them would
        silently change the request."""
        p = {}
        _put(p, limit=0, subsume=False, min_score=0.0, tags=[], name="")
        self.assertEqual(p, {"limit": 0, "subsume": False, "min_score": 0.0,
                             "tags": [], "name": ""})

    def test_extra_reaches_the_wire(self):
        """The escape hatch for a field a newer server understands."""
        with FakeServer(ok()) as srv:
            with AegisClient(port=srv.port) as db:
                db.search(query="x", some_future_field=7)
            self.assertEqual(srv.requests[0]["some_future_field"], 7)


if __name__ == "__main__":
    unittest.main()
