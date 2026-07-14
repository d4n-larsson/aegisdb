#!/usr/bin/env python3
"""Prometheus exporter for AegisDB.

A stateless sidecar: on each Prometheus scrape it opens one TCP connection to
the server, issues a single ``stats`` request (NDJSON, one line in / one line
out — the same wire protocol the Python integration speaks), and translates the
response into the Prometheus text exposition format served at ``/metrics``.

The ``stats`` operation is admin-scoped, so when the server enforces auth the
exporter must be given an admin (global) token via ``AEGIS_AUTH_TOKEN``. Any
failure to scrape (unreachable, timeout, UNAUTHORIZED, malformed) is reported as
``aegisdb_up 0`` rather than an HTTP error, so a scrape always succeeds and you
alert on ``aegisdb_up == 0``.

Dependency-free: only the Python standard library, matching the project's
vendor-don't-depend convention. Run directly (``python3 aegis_exporter.py``) or
via the container image; configuration is entirely through environment
variables (see ``_config`` below and the README).
"""
from __future__ import annotations

import json
import os
import socket
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


# --------------------------------------------------------------------------- #
# Configuration (environment variables, all optional except the token when the
# server enforces auth).
# --------------------------------------------------------------------------- #
class Config:
    def __init__(self, env=None):
        env = env if env is not None else os.environ
        self.host = env.get("AEGIS_HOST", "127.0.0.1")
        self.port = int(env.get("AEGIS_PORT", "9470"))
        self.auth_token = env.get("AEGIS_AUTH_TOKEN", "")
        self.exporter_bind = env.get("AEGIS_EXPORTER_BIND", "0.0.0.0")
        self.exporter_port = int(env.get("AEGIS_EXPORTER_PORT", "9471"))
        self.connect_timeout = int(env.get("AEGIS_CONNECT_TIMEOUT_MS", "500")) / 1000.0
        self.read_timeout = int(env.get("AEGIS_READ_TIMEOUT_MS", "2000")) / 1000.0


class ScrapeError(Exception):
    """The server could not be scraped (unreachable, timeout, auth, malformed)."""


def fetch_stats(cfg: Config) -> dict:
    """One ``stats`` round trip. Raises ScrapeError on any failure."""
    payload = {"operation": "stats"}
    if cfg.auth_token:
        payload["token"] = cfg.auth_token
    line = (json.dumps(payload) + "\n").encode("utf-8")
    try:
        with socket.create_connection((cfg.host, cfg.port),
                                      timeout=cfg.connect_timeout) as sock:
            sock.settimeout(cfg.read_timeout)
            sock.sendall(line)
            buf = bytearray()
            while not buf.endswith(b"\n"):
                chunk = sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
    except (OSError, socket.timeout) as exc:
        raise ScrapeError(str(exc)) from exc
    if not buf:
        raise ScrapeError("empty response")
    try:
        resp = json.loads(buf.decode("utf-8"))
    except ValueError as exc:
        raise ScrapeError(f"malformed response: {exc}") from exc
    if not resp.get("ok"):
        err = resp.get("error") or {}
        raise ScrapeError(err.get("code") or "stats request rejected")
    return resp


# --------------------------------------------------------------------------- #
# Rendering: stats JSON -> Prometheus text exposition format.
# --------------------------------------------------------------------------- #
def _fmt(v) -> str:
    """Render a metric value. Bools -> 1/0; ints stay integral; floats use a
    round-trippable repr (Prometheus wants a plain float, no thousands sep)."""
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, int):
        return str(v)
    return repr(float(v))


def _esc_label(v: str) -> str:
    """Escape a label value per the exposition format (backslash, quote, LF)."""
    return str(v).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


class Exposition:
    """Accumulates metric families and emits them with one HELP/TYPE header
    each, samples grouped under their family."""

    def __init__(self):
        self._families = []  # list of (name, type, help, [(labels, value)])

    def add(self, name, mtype, help_text, samples):
        # samples: list of (labels_dict, value); skip empty families entirely.
        samples = [s for s in samples if s is not None]
        if samples:
            self._families.append((name, mtype, help_text, samples))

    def gauge(self, name, help_text, value, labels=None):
        if value is None:
            return
        self.add(name, "gauge", help_text, [(labels or {}, value)])

    def counter(self, name, help_text, value, labels=None):
        if value is None:
            return
        self.add(name, "counter", help_text, [(labels or {}, value)])

    def render(self) -> str:
        out = []
        for name, mtype, help_text, samples in self._families:
            out.append(f"# HELP {name} {help_text}")
            out.append(f"# TYPE {name} {mtype}")
            for labels, value in samples:
                if labels:
                    inner = ",".join(f'{k}="{_esc_label(v)}"'
                                     for k, v in labels.items())
                    out.append(f"{name}{{{inner}}} {_fmt(value)}")
                else:
                    out.append(f"{name} {_fmt(value)}")
        return "\n".join(out) + "\n"


def render(stats: dict, *, up: bool = True, scrape_seconds: float | None = None,
           error: str | None = None) -> str:
    """Translate a ``stats`` response into exposition text. When ``up`` is
    False, ``stats`` is ignored and only the liveness/self metrics are emitted
    (with an ``aegisdb_scrape_error`` info metric carrying the reason)."""
    e = Exposition()
    e.gauge("aegisdb_up",
            "1 if the last stats scrape succeeded, 0 otherwise.", up)
    if scrape_seconds is not None:
        e.gauge("aegisdb_scrape_duration_seconds",
                "Duration of the stats scrape in seconds.", scrape_seconds)
    if not up:
        if error:
            e.add("aegisdb_scrape_error",
                  "gauge", "Always 1; the label carries the last scrape error.",
                  [({"error": error}, 1)])
        return e.render()

    # ---- server info (labels, constant 1) ----
    e.add("aegisdb_info", "gauge",
          "Server build/config info; constant 1, dimensions in labels.",
          [({"version": str(stats.get("version", "")),
             "phase": str(stats.get("phase", "")),
             "durability": str(stats.get("durability", ""))}, 1)])

    # ---- top-level gauges ----
    if "uptime_ms" in stats:
        e.gauge("aegisdb_uptime_seconds", "Server uptime in seconds.",
                stats["uptime_ms"] / 1000.0)
    e.gauge("aegisdb_records", "Live (non-tombstone) persisted records.",
            stats.get("records"))
    e.gauge("aegisdb_tombstones", "Deleted-but-not-yet-compacted records.",
            stats.get("tombstones"))
    e.gauge("aegisdb_log_bytes", "Append-only log size in bytes.",
            stats.get("log_bytes"))
    e.gauge("aegisdb_log_flush_pending",
            "1 if durable writes are awaiting an fsync.",
            stats.get("log_flush_pending"))
    e.gauge("aegisdb_next_id", "Next persisted record id to be allocated.",
            stats.get("next_id"))

    # ---- index entry counts ----
    idx = stats.get("indexes") or {}
    e.add("aegisdb_index_entries", "gauge",
          "Entry count per in-memory index.",
          [({"index": k}, v) for k, v in idx.items()])

    # ---- per-index resident bytes ----
    mem = stats.get("memory") or {}
    byte_samples = []
    for key, label in (("hash_bytes", "hash"), ("time_bytes", "time"),
                       ("tag_bytes", "tag"), ("semantic_bytes", "semantic")):
        if key in mem:
            byte_samples.append(({"index": label}, mem[key]))
    e.add("aegisdb_index_bytes", "gauge",
          "Approximate resident bytes per in-memory index.", byte_samples)
    e.gauge("aegisdb_index_bytes_total",
            "Total approximate resident index bytes.",
            mem.get("index_bytes_total"))
    e.gauge("aegisdb_index_bytes_limit",
            "Configured --max-index-bytes backpressure cap (0 = unlimited).",
            mem.get("index_bytes_limit"))

    # ---- operational counters ----
    m = stats.get("metrics") or {}
    e.counter("aegisdb_requests_total", "Total dispatched requests.",
              m.get("requests"))
    e.counter("aegisdb_errors_total", "Responses with ok:false.",
              m.get("errors"))
    e.counter("aegisdb_unauthorized_total", "Auth rejections.",
              m.get("unauthorized"))
    if "dispatch_micros" in m:
        e.counter("aegisdb_dispatch_seconds_total",
                  "Cumulative in-dispatch time in seconds.",
                  m["dispatch_micros"] / 1e6)
    by_op = m.get("by_op") or {}
    e.add("aegisdb_requests_by_op_total", "counter",
          "Requests per operation.",
          [({"op": op}, v) for op, v in by_op.items()])

    # ---- per-tenant usage (only present when quotas are configured) ----
    tenants = stats.get("tenants") or []
    e.add("aegisdb_tenant_records", "gauge",
          "Live records per tenant namespace.",
          [({"namespace": t.get("namespace", "")}, t.get("records", 0))
           for t in tenants])
    e.add("aegisdb_tenant_bytes", "gauge",
          "Live payload bytes per tenant namespace.",
          [({"namespace": t.get("namespace", "")}, t.get("bytes", 0))
           for t in tenants])

    # ---- replication posture (only when this node participates) ----
    rep = stats.get("replication")
    if rep:
        role = {"role": rep.get("role", "")}
        e.gauge("aegisdb_replication_connected",
                "1 if a replica is connected to its primary.",
                rep.get("connected"), labels=role)
        e.gauge("aegisdb_replication_applied_offset_bytes",
                "Log offset applied on a replica.",
                rep.get("applied_offset"), labels=role)
        e.gauge("aegisdb_replication_primary_offset_bytes",
                "Primary log offset as last seen by a replica.",
                rep.get("primary_offset"), labels=role)
        e.gauge("aegisdb_replication_lag_bytes",
                "Replica lag behind the primary in bytes.",
                rep.get("lag_bytes"), labels=role)
        e.gauge("aegisdb_replication_replicas",
                "Replicas connected to this primary.",
                rep.get("replicas"), labels=role)

    return e.render()


def scrape_text(cfg: Config) -> str:
    """Fetch + render one scrape, converting failures into aegisdb_up 0."""
    start = time.monotonic()
    try:
        stats = fetch_stats(cfg)
    except ScrapeError as exc:
        return render({}, up=False,
                      scrape_seconds=time.monotonic() - start, error=str(exc))
    return render(stats, up=True, scrape_seconds=time.monotonic() - start)


# --------------------------------------------------------------------------- #
# HTTP server.
# --------------------------------------------------------------------------- #
CONTENT_TYPE = "text/plain; version=0.0.4; charset=utf-8"


def make_handler(cfg: Config):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def _send(self, code, body: bytes, content_type="text/plain"):
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def do_GET(self):
            if self.path in ("/metrics", "/metrics/"):
                body = scrape_text(cfg).encode("utf-8")
                self._send(200, body, CONTENT_TYPE)
            elif self.path in ("/", ""):
                self._send(200,
                           b'<html><body><a href="/metrics">/metrics</a>'
                           b"</body></html>\n", "text/html")
            elif self.path in ("/healthz", "/-/healthy"):
                self._send(200, b"ok\n")
            else:
                self._send(404, b"not found\n")

        do_HEAD = do_GET

        def log_message(self, fmt, *args):  # keep stdout clean; log to stderr
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    return Handler


def main(argv=None):
    cfg = Config()
    server = ThreadingHTTPServer((cfg.exporter_bind, cfg.exporter_port),
                                 make_handler(cfg))
    sys.stderr.write(
        f"aegis-exporter: scraping {cfg.host}:{cfg.port}, serving /metrics on "
        f"{cfg.exporter_bind}:{cfg.exporter_port}"
        f"{' (auth token set)' if cfg.auth_token else ''}\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())