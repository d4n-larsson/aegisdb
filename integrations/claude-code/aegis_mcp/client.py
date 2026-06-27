"""AegisDB NDJSON/TCP client (T005) + startup checks (T010).

One request per call: open a connection, send a single JSON line, read a single
JSON line, close. AegisDB supports pipelining but a fresh connection per request
keeps the client simple and stateless, which is fine for the integration's low
call volume. Connection/timeout failures raise ``AegisUnavailable`` so callers
can degrade gracefully (FR-009).
"""
from __future__ import annotations

import json
import socket


class AegisUnavailable(Exception):
    """The backend could not be reached or did not respond in time."""


class AegisClient:
    def __init__(self, host="127.0.0.1", port=9470,
                 connect_timeout_ms=500, read_timeout_ms=1000, auth_token=""):
        self.host = host
        self.port = int(port)
        self.connect_timeout = connect_timeout_ms / 1000.0
        self.read_timeout = read_timeout_ms / 1000.0
        self.auth_token = auth_token or ""

    def request(self, payload: dict, read_timeout_ms: int | None = None) -> dict:
        """Send one request, return the parsed JSON response.

        Raises AegisUnavailable on any connection, timeout, or protocol error.
        """
        read_timeout = (read_timeout_ms / 1000.0) if read_timeout_ms is not None \
            else self.read_timeout
        if self.auth_token:
            payload.setdefault("token", self.auth_token)
        line = (json.dumps(payload) + "\n").encode("utf-8")
        try:
            with socket.create_connection((self.host, self.port),
                                          timeout=self.connect_timeout) as sock:
                sock.settimeout(read_timeout)
                sock.sendall(line)
                buf = bytearray()
                while not buf.endswith(b"\n"):
                    chunk = sock.recv(65536)
                    if not chunk:
                        break
                    buf += chunk
        except (OSError, socket.timeout) as exc:
            raise AegisUnavailable(str(exc)) from exc
        if not buf:
            raise AegisUnavailable("empty response")
        try:
            return json.loads(buf.decode("utf-8"))
        except ValueError as exc:
            raise AegisUnavailable(f"malformed response: {exc}") from exc

    def ping(self) -> dict:
        return self.request({"operation": "ping"})

    def available(self) -> bool:
        try:
            resp = self.ping()
        except AegisUnavailable:
            return False
        return bool(resp.get("ok"))


def check_startup(client: AegisClient, config) -> dict:
    """Best-effort startup check (T010).

    Returns a dict describing reachability and a dimension warning. AegisDB's
    ``ping`` does not expose the server's configured embedding dimension, so a
    true server-vs-client dimension mismatch surfaces as a clear error on the
    first embedded operation (translated in results.py). Here we validate what
    we can locally and report reachability without ever raising — an unreachable
    backend must not prevent the integration from starting (FR-009).
    """
    info = {"reachable": False, "version": None, "phase": None, "warnings": []}
    try:
        resp = client.ping()
        info["reachable"] = bool(resp.get("ok"))
        info["version"] = resp.get("version")
        info["phase"] = resp.get("phase")
    except AegisUnavailable as exc:
        info["warnings"].append(f"AegisDB unreachable at startup: {exc}")
    if config.embedding_mode != "none" and config.embedding_dimensions <= 0:
        info["warnings"].append("embedding_dimensions must be positive")
    return info