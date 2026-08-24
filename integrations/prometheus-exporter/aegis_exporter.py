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
        # Distillation lag (ROADMAP 3.3). Off by default, and deliberately so:
        # unlike `stats`, which is a cheap read of counters the server already
        # holds, the backlog is a filtered scan. Measured on a 50k-record
        # corpus: stats 3.4 ms, the two backlog queries 44 ms each — a 26x
        # scrape, growing with the corpus. An operator who does not run the
        # distiller should not pay that every 15 seconds.
        self.distillation = env.get("AEGIS_EXPORTER_DISTILLATION",
                                    "").lower() in ("1", "true", "yes", "on")
        # Recomputed at most this often regardless of scrape interval, so a
        # tight Prometheus interval cannot turn a scan into a busy loop.
        self.distillation_interval = float(
            env.get("AEGIS_EXPORTER_DISTILLATION_INTERVAL_SEC", "60"))
        # The distiller's own eligibility thresholds, read from the *same*
        # environment variables `aegisdb-summarize` reads. They are duplicated
        # here — the server does not know them, so the backlog cannot be
        # computed without them — and that duplication is made visible rather
        # than hidden: both are exported as gauges, so a dashboard shows what
        # the backlog was computed with and a mismatch with the job's config is
        # something an operator can see instead of a number that quietly lies.
        self.summary_min_age_ms = int(
            env.get("AEGIS_SUMMARY_MIN_AGE_MS", "604800000"))
        self.summary_max_importance = float(
            env.get("AEGIS_SUMMARY_MAX_IMPORTANCE", "0.6"))


class ScrapeError(Exception):
    """The server could not be scraped (unreachable, timeout, auth, malformed)."""


def _request(cfg: Config, payload: dict) -> dict:
    """One round trip. Raises ScrapeError on any failure."""
    payload = dict(payload)
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
        raise ScrapeError(err.get("code") or "request rejected")
    return resp


def fetch_stats(cfg: Config) -> dict:
    """One ``stats`` round trip."""
    return _request(cfg, {"operation": "stats"})


def fetch_distillation(cfg: Config, now_ms: int) -> dict:
    """How far behind the distiller is, in records and in time.

    Two questions, because either alone misleads. *Backlog* says how much work
    is waiting; *oldest* says how long the eldest piece of it has been waiting,
    which is the number that distinguishes "a steady trickle" from "the job
    stopped running a month ago".

    Neither is derivable from `stats`: the server does not know the distiller's
    thresholds, so eligibility has to be expressed as a query. It is the same
    predicate `aegisdb-summarize` selects on — episodic, older than the minimum
    age, at or below the importance ceiling.

    `capped` matters. Past `--query-scan-cap` the server counts over a bounded
    view, which makes the backlog a *floor* rather than the number. Reported as
    its own gauge instead of being folded in, because a floor presented as a
    total is exactly the sort of quiet lie an alert gets built on.
    """
    cutoff = max(0, now_ms - cfg.summary_min_age_ms)
    common = {"type": "episodic", "end_time": cutoff,
              "max_importance": cfg.summary_max_importance}
    counted = _request(cfg, {"operation": "count", **common})
    oldest = _request(cfg, {"operation": "search", "order": "oldest",
                            "top_k": 1, **common})
    recs = oldest.get("records") or []
    # Age from `updated`, not `created`: the distiller selects on the same
    # field, so a record amended yesterday is not eligible however old it is.
    age_ms = 0
    if recs:
        stamp = recs[0].get("updated") or recs[0].get("created") or now_ms
        age_ms = max(0, now_ms - int(stamp))
    return {"backlog": int(counted.get("count") or 0),
            "capped": bool(counted.get("capped")),
            "oldest_age_ms": age_ms}


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

    def histogram(self, name, help_text, buckets, count, total):
        """Emit a Prometheus histogram: `_bucket{le=...}` samples followed by
        `_sum` and `_count`, all under one HELP/TYPE header carrying the **base**
        name — the exposition format puts the suffixes on samples only.

        `buckets` maps an upper bound (in the metric's own unit) to a
        **cumulative** count, which is the shape the server already reports."""
        if count is None or not buckets:
            return
        # +Inf must sort last; the finite bounds ascend numerically.
        finite = sorted((b for b in buckets if b != "+Inf"), key=float)
        samples = [({"le": _fmt(float(b))}, buckets[b], "_bucket")
                   for b in finite]
        samples.append(({"le": "+Inf"}, buckets.get("+Inf", count), "_bucket"))
        samples.append(({}, total, "_sum"))
        samples.append(({}, count, "_count"))
        self._families.append((name, "histogram", help_text, samples))

    def render(self) -> str:
        out = []
        for name, mtype, help_text, samples in self._families:
            out.append(f"# HELP {name} {help_text}")
            out.append(f"# TYPE {name} {mtype}")
            for sample in samples:
                # A sample may carry a third element: a name suffix, which a
                # histogram uses for _bucket/_sum/_count under one header.
                labels, value = sample[0], sample[1]
                sname = name + (sample[2] if len(sample) > 2 else "")
                if labels:
                    inner = ",".join(f'{k}="{_esc_label(v)}"'
                                     for k, v in labels.items())
                    out.append(f"{sname}{{{inner}}} {_fmt(value)}")
                else:
                    out.append(f"{sname} {_fmt(value)}")
        return "\n".join(out) + "\n"


def render(stats: dict, *, up: bool = True, scrape_seconds: float | None = None,
           error: str | None = None, distillation: dict | None = None,
           cfg: Config | None = None) -> str:
    """Translate a ``stats`` response into exposition text. When ``up`` is
    False, ``stats`` is ignored and only the liveness/self metrics are emitted
    (with an ``aegisdb_scrape_error`` info metric carrying the reason).

    ``distillation`` is the extra block from `fetch_distillation`, present only
    when that feature is enabled and the last computation succeeded."""
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
    # Derived from whatever the server reports rather than a hardcoded list, so
    # a newly added index shows up here without a matching edit. The hardcoded
    # version had silently omitted lexical_bytes and usage_bytes, which made the
    # per-index breakdown fail to add up to index_bytes_total. The two summary
    # keys are excluded by suffix: they end in _total/_limit, not _bytes.
    mem = stats.get("memory") or {}
    byte_samples = [({"index": k[: -len("_bytes")]}, v)
                    for k, v in mem.items() if k.endswith("_bytes")]
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

    # ---- recall latency distribution ----
    # Absent until the server has served a search, so a fresh server reports no
    # histogram rather than an all-zero one. Bounds are microseconds on the wire
    # and converted to seconds here, which is the Prometheus base unit.
    rl = m.get("recall_latency") or {}
    if rl.get("count"):
        buckets = {
            "+Inf" if b == "+Inf" else str(float(b) / 1e6): v
            for b, v in (rl.get("buckets") or {}).items()
        }
        e.histogram("aegisdb_recall_latency_seconds",
                    "Latency distribution of the search (recall) operation.",
                    buckets, rl["count"], rl.get("micros_total", 0) / 1e6)

    # ---- memory-quality outcomes (dedup / decay / erase) ----
    if "memories_merged" in m:
        e.counter("aegisdb_memories_merged_total",
                  "Records merged away by consolidate (dedup).",
                  m.get("memories_merged"))
    if "memories_forgotten" in m:
        e.counter("aegisdb_memories_forgotten_total",
                  "Records aged out by forget (decay).",
                  m.get("memories_forgotten"))
    if "memories_purged" in m:
        e.counter("aegisdb_memories_purged_total",
                  "Records erased by purge (right-to-be-forgotten).",
                  m.get("memories_purged"))

    # ---- distillation lag (opt-in; ROADMAP 3.3) ----
    # Absent entirely unless enabled, so a dashboard panel showing nothing is
    # distinguishable from a backlog of zero — the two mean opposite things and
    # emitting 0 for "not measured" would make the healthy state and the
    # unmeasured state look identical.
    if distillation is not None:
        e.gauge("aegisdb_distillation_backlog",
                "Records eligible for distillation but not yet summarized.",
                distillation.get("backlog", 0))
        e.gauge("aegisdb_distillation_backlog_capped",
                "1 when the backlog count hit the server's scan cap and is "
                "therefore a floor, not a total.",
                1 if distillation.get("capped") else 0)
        e.gauge("aegisdb_distillation_oldest_age_seconds",
                "Age of the oldest record still awaiting distillation; 0 when "
                "the backlog is empty.",
                distillation.get("oldest_age_ms", 0) / 1000.0)
    if cfg is not None and cfg.distillation:
        # The thresholds the backlog above was computed with. The server does
        # not know them, so they are the exporter's copy of the distiller's
        # config — exported so a mismatch between the two is visible on the
        # dashboard rather than silently wrong in the number.
        e.gauge("aegisdb_distillation_min_age_seconds",
                "Minimum record age the backlog counts as eligible "
                "(AEGIS_SUMMARY_MIN_AGE_MS).",
                cfg.summary_min_age_ms / 1000.0)
        e.gauge("aegisdb_distillation_max_importance",
                "Importance ceiling the backlog counts as eligible "
                "(AEGIS_SUMMARY_MAX_IMPORTANCE).",
                cfg.summary_max_importance)

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


# Last distillation result and when it was computed, so the scan runs on its
# own cadence rather than Prometheus's. Module-level because one process serves
# one server; a per-cfg cache would be ceremony for a case that does not exist.
_distillation_cache: dict = {"at": None, "value": None}


def _distillation(cfg: Config, now_ms: int) -> dict | None:
    """The cached backlog, recomputed at most every `distillation_interval`.

    A failure keeps the previous value rather than clearing it: the backlog is
    a slow-moving number, and dropping it to nothing on one timed-out scan
    would look exactly like the distiller having caught up.
    """
    if not cfg.distillation:
        return None
    now = time.monotonic()
    at = _distillation_cache["at"]
    if at is not None and (now - at) < cfg.distillation_interval:
        return _distillation_cache["value"]
    try:
        value = fetch_distillation(cfg, now_ms)
    except ScrapeError:
        # Stamp the attempt so a persistently failing query is retried on the
        # interval rather than on every scrape.
        _distillation_cache["at"] = now
        return _distillation_cache["value"]
    _distillation_cache["at"] = now
    _distillation_cache["value"] = value
    return value


def scrape_text(cfg: Config) -> str:
    """Fetch + render one scrape, converting failures into aegisdb_up 0."""
    start = time.monotonic()
    try:
        stats = fetch_stats(cfg)
    except ScrapeError as exc:
        return render({}, up=False,
                      scrape_seconds=time.monotonic() - start, error=str(exc),
                      cfg=cfg)
    # After `stats`, so a server that is up but slow still reports liveness and
    # counters even if the backlog query times out.
    lag = _distillation(cfg, int(time.time() * 1000))
    return render(stats, up=True, scrape_seconds=time.monotonic() - start,
                  distillation=lag, cfg=cfg)


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