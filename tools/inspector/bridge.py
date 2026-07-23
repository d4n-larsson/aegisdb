#!/usr/bin/env python3
"""Memory-inspection bridge for AegisDB (ROADMAP Horizon 1.3).

A browser can't speak AegisDB's newline-delimited-JSON-over-TCP wire protocol, so
this tiny local bridge does three things:

  1. serves the self-contained inspector UI (index.html) at `/`,
  2. proxies an allow-listed set of wire operations to the DB (POST /api/query),
     injecting the auth token server-side so it never lives in the browser,
  3. embeds query text (POST /api/embed) so the UI can run semantic searches with
     the per-hit `explain` breakdown without shipping an embedder to the browser.

Stdlib only. Binds to localhost by default — it hands out an authenticated proxy
to your database, so do not expose it to a network.

    python3 tools/inspector/bridge.py --aegis-port 9470 [--token <tok>] \
        [--embedder hashing|command|none] [--embedder-cmd '<prog>'] \
        [--embedding-dim 384] [--http-port 8600]

Then open http://127.0.0.1:8600/.
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "eval"))

# Read-and-edit operations an inspector legitimately needs. Deliberately excludes
# token administration, snapshot, and bulk writes — the bridge is a lens, not a
# general control plane.
ALLOWED_OPS = {"ping", "stats", "search", "get", "count", "update", "delete"}

CONFIG = {}  # filled by main()


def wire_request(payload: dict) -> dict:
    """Send one request to AegisDB over TCP and return the parsed response."""
    if CONFIG["token"]:
        payload.setdefault("token", CONFIG["token"])
    line = (json.dumps(payload) + "\n").encode()
    with socket.create_connection((CONFIG["host"], CONFIG["port"]), timeout=10) as s:
        s.sendall(line)
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    return json.loads(data.decode())


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, obj, ctype="application/json"):
        body = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):  # quiet
        pass

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            try:
                with open(os.path.join(HERE, "index.html"), "rb") as fh:
                    self._send(200, fh.read(), "text/html; charset=utf-8")
            except OSError:
                self._send(500, {"error": "index.html not found"})
        elif self.path == "/aegis.css":
            # The shared visual identity, from the repo's single source of truth
            # (site/aegis.css) so the inspector and the landing site never drift.
            try:
                with open(os.path.join(HERE, "..", "..", "site", "aegis.css"), "rb") as fh:
                    self._send(200, fh.read(), "text/css; charset=utf-8")
            except OSError:
                self._send(404, {"error": "aegis.css not found"})
        elif self.path == "/api/config":
            self._send(200, {
                "embedder": CONFIG["embedder_name"],
                "embedding_dim": CONFIG["embedding_dim"],
                "aegis": f'{CONFIG["host"]}:{CONFIG["port"]}',
            })
        else:
            self._send(404, {"error": "not found"})

    def _read_json(self):
        n = int(self.headers.get("Content-Length", 0))
        return json.loads(self.rfile.read(n).decode()) if n else {}

    def do_POST(self):
        try:
            body = self._read_json()
        except (ValueError, json.JSONDecodeError):
            return self._send(400, {"error": "invalid JSON body"})

        if self.path == "/api/query":
            op = body.get("operation")
            if op not in ALLOWED_OPS:
                return self._send(403, {"error": f"operation '{op}' not allowed by the inspector"})
            try:
                return self._send(200, wire_request(body))
            except OSError as e:
                return self._send(502, {"error": f"cannot reach AegisDB: {e}"})

        if self.path == "/api/embed":
            if CONFIG["embedder"] is None:
                return self._send(400, {"error": "no embedder configured (start the bridge with --embedder)"})
            text = body.get("text", "")
            try:
                return self._send(200, {"embedding": CONFIG["embedder"](text)})
            except Exception as e:  # embedder command failures, etc.
                return self._send(500, {"error": f"embed failed: {e}"})

        self._send(404, {"error": "not found"})


def main():
    ap = argparse.ArgumentParser(description="AegisDB memory-inspection bridge")
    ap.add_argument("--http-port", type=int, default=8600)
    ap.add_argument("--bind", default="127.0.0.1", help="HTTP bind address (keep localhost)")
    ap.add_argument("--aegis-host", default="127.0.0.1")
    ap.add_argument("--aegis-port", type=int, default=9470)
    ap.add_argument("--token", default=os.environ.get("AEGIS_TOKEN"),
                    help="auth token (or set AEGIS_TOKEN)")
    ap.add_argument("--embedder", default="subword",
                    choices=["subword", "hashing", "command", "none"])
    ap.add_argument("--embedder-cmd", default=None)
    ap.add_argument("--embedding-dim", type=int, default=384)
    args = ap.parse_args()

    embedder = None
    if args.embedder != "none":
        from embedders import resolve_embedder
        embedder = resolve_embedder(args.embedder, args.embedding_dim, args.embedder_cmd)

    CONFIG.update(host=args.aegis_host, port=args.aegis_port, token=args.token,
                  embedder=embedder, embedder_name=args.embedder,
                  embedding_dim=args.embedding_dim)

    srv = ThreadingHTTPServer((args.bind, args.http_port), Handler)
    print(f"AegisDB inspector: http://{args.bind}:{args.http_port}/  "
          f"-> aegisdb {args.aegis_host}:{args.aegis_port}  "
          f"(embedder: {args.embedder}, dim {args.embedding_dim})")
    print("Ctrl-C to stop.")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()