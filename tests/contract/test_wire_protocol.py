#!/usr/bin/env python3
"""Wire-protocol contract tests for AegisDB (see docs/wire-protocol.md).

Launches a real server over TCP and validates request handling, response
schemas, error paths, and phase gating. Exits non-zero on any failure so it can
be driven by CTest / make.

Usage:
    python3 tests/contract/test_wire_protocol.py [path/to/aegisdb]
"""
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

FAILURES = []


def check(cond, msg):
    if cond:
        print(f"  ok   - {msg}")
    else:
        print(f"  FAIL - {msg}")
        FAILURES.append(msg)


class Server:
    def __init__(self, binary, port, phase=4, auth_token=None, token_lines=None,
                 io_threads=None, extra_args=None, datadir=None,
                 expect_exit=False):
        self.binary = binary
        self.port = port
        self.phase = phase
        self.auth_token = auth_token
        self.token_lines = token_lines  # lines for --auth-token-file
        self.io_threads = io_threads
        self.extra_args = extra_args or []  # arbitrary extra CLI flags
        self.proc = None
        # A caller-provided datadir persists across restarts (recovery tests);
        # otherwise a throwaway one is created per instance.
        self.datadir = datadir or tempfile.mkdtemp(prefix="aegis_contract_")
        self.expect_exit = expect_exit  # server is expected to fail to start

    def __enter__(self):
        args = [self.binary, "--data-dir", self.datadir, "--port",
                str(self.port), "--phase", str(self.phase)]
        args += self.extra_args
        if self.io_threads is not None:
            args += ["--io-threads", str(self.io_threads)]
        if self.auth_token:
            args += ["--auth-token", self.auth_token]
        if self.token_lines:
            tf = os.path.join(self.datadir, "tokens")
            with open(tf, "w") as fh:
                fh.write("\n".join(self.token_lines) + "\n")
            args += ["--auth-token-file", tf]
        self.proc = subprocess.Popen(
            args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        # A server expected to reject its config (e.g. wrong key) should exit
        # nonzero rather than start listening.
        if self.expect_exit:
            try:
                self.rc = self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.rc = None
            return self
        # Wait for the listener to accept connections.
        for _ in range(50):
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    return self
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("server did not start")

    def graceful_stop(self):
        """SIGTERM + wait, so the server runs its clean shutdown (which writes a
        checkpoint). Safe to call once; __exit__ then no-ops."""
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(timeout=10)

    def __exit__(self, *a):
        if self.proc and self.proc.poll() is None:
            # Under coverage (`make coverage`) stop gracefully so the server runs
            # its clean shutdown and gcov flushes its data; SIGKILL would drop it.
            if os.environ.get("AEGIS_COV"):
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=10)
                    return
                except subprocess.TimeoutExpired:
                    pass
            self.proc.kill()
            self.proc.wait()

    def req(self, payload):
        with socket.create_connection(("127.0.0.1", self.port), timeout=2) as s:
            s.sendall((json.dumps(payload) + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
        return json.loads(data.decode())


def test_full_phase(binary, port):
    print("[phase 4: full protocol]")
    with Server(binary, port, phase=4) as srv:
        # ping
        r = srv.req({"operation": "ping"})
        check(r.get("ok") is True, "ping ok")
        check(isinstance(r.get("version"), str) and r.get("version"),
              "ping reports version")
        check("phase" in r, "ping reports phase")

        # request_id echo
        r = srv.req({"operation": "ping", "request_id": "abc-123"})
        check(r.get("request_id") == "abc-123", "request_id is echoed")

        # insert episodic -> record with server-assigned id + timestamps
        r = srv.req({"operation": "insert", "type": "episodic",
                     "tags": ["user", "preference"], "data": "User likes coffee",
                     "importance": 0.7})
        check(r.get("ok") is True, "insert episodic ok")
        rec = r.get("record", {})
        check(isinstance(rec.get("id"), int) and rec["id"] > 0, "insert assigns id")
        check(rec.get("created") == rec.get("updated"), "episodic created==updated")
        eid = rec.get("id")

        # get by id
        r = srv.req({"operation": "get", "id": eid})
        check(r.get("ok") is True and r["record"]["data"] == "User likes coffee",
              "get returns inserted record")

        # NOT_FOUND for unknown id, no side effects
        r = srv.req({"operation": "get", "id": 9999999})
        check(r.get("ok") is False and r["error"]["code"] == "NOT_FOUND",
              "get unknown id -> NOT_FOUND")

        # INVALID_REQUEST: missing required 'data' on insert
        r = srv.req({"operation": "insert", "type": "episodic"})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "insert missing data -> INVALID_REQUEST")

        # INVALID_REQUEST: unknown operation
        r = srv.req({"operation": "definitely_not_an_op"})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "unknown operation -> INVALID_REQUEST")

        # IMMUTABLE: update on episodic
        r = srv.req({"operation": "update", "id": eid, "data": "changed"})
        check(r.get("ok") is False and r["error"]["code"] == "IMMUTABLE",
              "update episodic -> IMMUTABLE")

        # semantic insert + update succeeds
        r = srv.req({"operation": "insert", "type": "semantic", "data": "sky is blue"})
        sid = r["record"]["id"]
        r = srv.req({"operation": "update", "id": sid, "data": "sky is azure",
                     "confidence": 0.9})
        check(r.get("ok") is True and r["record"]["data"] == "sky is azure",
              "update semantic succeeds")

        # search returns records + total
        r = srv.req({"operation": "search", "start_time": 0,
                     "end_time": 9999999999999, "top_k": 10})
        check(r.get("ok") is True and isinstance(r.get("records"), list)
              and "total" in r, "search returns records[] and total")

        # empty result is well-formed
        r = srv.req({"operation": "search", "tags": ["no_such_tag_xyz"],
                     "match": "all", "top_k": 10})
        check(r.get("ok") is True and r.get("total") == 0
              and r.get("records") == [], "search no match -> empty result")


def test_stats(binary, port):
    print("[phase 4: stats]")
    with Server(binary, port, phase=4) as srv:
        r = srv.req({"operation": "stats"})
        check(r.get("ok") is True, "stats ok")
        check(isinstance(r.get("version"), str) and r.get("version"),
              "stats reports version")
        check(r.get("durability") in ("sync", "batch", "interval"),
              "stats reports durability mode")
        for field in ("records", "tombstones", "log_bytes", "next_id"):
            check(field in r, f"stats reports {field}")
        check(isinstance(r.get("log_flush_pending"), bool),
              "stats reports log_flush_pending bool")
        check(isinstance(r.get("indexes"), dict)
              and {"time", "tags", "semantic", "working"} <= set(r["indexes"]),
              "stats reports per-index counts")
        # per-index memory estimates, for OOM monitoring
        mem = r.get("memory")
        check(isinstance(mem, dict)
              and {"hash_bytes", "time_bytes", "tag_bytes", "semantic_bytes",
                   "index_bytes_total"} <= set(mem),
              "stats reports per-index memory bytes")
        check(mem["index_bytes_total"]
              >= mem["hash_bytes"] + mem["semantic_bytes"],
              "index_bytes_total sums the per-index figures")
        # inserting a vector grows the semantic memory estimate
        sem0 = srv.req({"operation": "stats"})["memory"]["semantic_bytes"]
        srv.req({"operation": "insert", "type": "semantic", "data": "vec",
                 "embedding": [0.1] * 384})  # default --embedding-dim 384
        sem1 = srv.req({"operation": "stats"})["memory"]["semantic_bytes"]
        check(sem1 > sem0, "semantic memory grows after inserting a vector")

        base = srv.req({"operation": "stats"})["records"]
        srv.req({"operation": "insert", "type": "episodic",
                 "tags": ["s"], "data": "counted"})
        r = srv.req({"operation": "stats"})
        check(r["records"] == base + 1, "stats records increments on insert")

        ins = srv.req({"operation": "insert", "type": "episodic",
                       "tags": ["s"], "data": "doomed"})
        srv.req({"operation": "delete", "id": ins["record"]["id"]})
        r = srv.req({"operation": "stats"})
        check(r["records"] == base + 1 and r["tombstones"] >= 1,
              "stats moves a deleted record to tombstones")

        # request_id is echoed on stats like any other op
        r = srv.req({"operation": "stats", "request_id": "stat-rid"})
        check(r.get("request_id") == "stat-rid", "stats echoes request_id")

        # operational metrics: monotonic counters + per-op breakdown
        m = srv.req({"operation": "stats"}).get("metrics", {})
        check(isinstance(m, dict)
              and {"requests", "errors", "unauthorized", "dispatch_micros",
                   "by_op"} <= set(m),
              "stats reports metrics object")
        req_before = m["requests"]
        searches_before = m["by_op"]["search"]
        errs_before = m["errors"]

        srv.req({"operation": "search", "tags": ["s"], "top_k": 5})
        srv.req({"operation": "bogus_op"})  # -> error
        m2 = srv.req({"operation": "stats"}).get("metrics", {})
        check(m2["requests"] >= req_before + 3, "requests counter advances")
        check(m2["by_op"]["search"] == searches_before + 1,
              "by_op.search counts the search")
        check(m2["errors"] >= errs_before + 1, "errors counter catches bogus op")

        # These ops used to be missing from the metric map and silently bucketed
        # into by_op.other (#187). Each must now have its own labelled counter,
        # and exercising it must advance that counter, not "other".
        labelled = ("history", "export", "consolidate", "forget", "purge")
        check(all(k in m2["by_op"] for k in labelled),
              "by_op has a labelled counter for history/export/consolidate/forget/purge")
        other_before = m2["by_op"]["other"]
        rec = srv.req({"operation": "insert", "type": "semantic",
                       "data": "metrics probe"})["record"]["id"]
        srv.req({"operation": "history", "id": rec})
        srv.req({"operation": "export"})
        srv.req({"operation": "consolidate"})
        srv.req({"operation": "forget", "older_than_ms": 1})
        srv.req({"operation": "purge", "namespace": "no-such-ns"})
        m3 = srv.req({"operation": "stats"}).get("metrics", {})
        for k in labelled:
            check(m3["by_op"][k] >= 1, f"by_op.{k} counts its op (not 'other')")
        check(m3["by_op"]["other"] == other_before,
              "labelled ops no longer inflate by_op.other")


def test_delete(binary, port):
    print("[phase 4: delete]")
    with Server(binary, port, phase=4) as srv:
        # INVALID_REQUEST: missing required 'id'
        r = srv.req({"operation": "delete"})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "delete missing id -> INVALID_REQUEST")

        # NOT_FOUND for unknown id
        r = srv.req({"operation": "delete", "id": 9999999})
        check(r.get("ok") is False and r["error"]["code"] == "NOT_FOUND",
              "delete unknown id -> NOT_FOUND")

        # delete an episodic record
        r = srv.req({"operation": "insert", "type": "episodic",
                     "tags": ["doomed"], "data": "delete me"})
        eid = r["record"]["id"]
        r = srv.req({"operation": "delete", "id": eid})
        check(r.get("ok") is True and r.get("id") == eid
              and r.get("deleted") is True, "delete episodic -> ok")

        # gone from get
        r = srv.req({"operation": "get", "id": eid})
        check(r.get("ok") is False and r["error"]["code"] == "NOT_FOUND",
              "get after delete -> NOT_FOUND")

        # idempotent: deleting again -> NOT_FOUND
        r = srv.req({"operation": "delete", "id": eid})
        check(r.get("ok") is False and r["error"]["code"] == "NOT_FOUND",
              "delete already-deleted -> NOT_FOUND")

        # gone from search (tag index dropped)
        r = srv.req({"operation": "search", "tags": ["doomed"],
                     "match": "all", "top_k": 10})
        check(r.get("ok") is True and r.get("total") == 0,
              "deleted record absent from search")

        # delete works on semantic records too
        r = srv.req({"operation": "insert", "type": "semantic", "data": "ephemeral"})
        sid = r["record"]["id"]
        r = srv.req({"operation": "delete", "id": sid})
        check(r.get("ok") is True and r.get("deleted") is True,
              "delete semantic -> ok")

        # deleting a relationship target removes it from traversal
        a = srv.req({"operation": "insert", "type": "semantic",
                     "data": "source"})["record"]["id"]
        b = srv.req({"operation": "insert", "type": "semantic",
                     "data": "target"})["record"]["id"]
        srv.req({"operation": "relate", "from_id": a, "to_id": b, "kind": "rel"})
        srv.req({"operation": "delete", "id": b})
        r = srv.req({"operation": "traverse", "id": a, "depth": 1})
        ids = [rec["id"] for rec in r.get("records", [])]
        check(r.get("ok") is True and b not in ids,
              "deleted target absent from traversal")


def test_auth(binary, port):
    print("[auth: token required]")
    with Server(binary, port, phase=4, auth_token="s3cret") as srv:
        # ping is exempt -> works with no token
        r = srv.req({"operation": "ping"})
        check(r.get("ok") is True, "ping works without a token")

        # a normal op with no token -> UNAUTHORIZED
        r = srv.req({"operation": "insert", "type": "episodic", "data": "x"})
        check(r.get("ok") is False and r["error"]["code"] == "UNAUTHORIZED",
              "insert without token -> UNAUTHORIZED")

        # wrong token -> UNAUTHORIZED
        r = srv.req({"operation": "insert", "type": "episodic", "data": "x",
                     "token": "nope"})
        check(r.get("ok") is False and r["error"]["code"] == "UNAUTHORIZED",
              "insert with wrong token -> UNAUTHORIZED")

        # correct token -> ok, and request_id still echoed on the gated path
        r = srv.req({"operation": "insert", "type": "episodic",
                     "data": "authed", "token": "s3cret", "request_id": "rid-1"})
        check(r.get("ok") is True, "insert with correct token -> ok")
        check(r.get("request_id") == "rid-1", "request_id echoed with auth")

        # unauthorized errors also echo request_id
        r = srv.req({"operation": "get", "id": 1, "request_id": "rid-2"})
        check(r.get("request_id") == "rid-2",
              "request_id echoed on UNAUTHORIZED")

        # stats is NOT exempt (unlike ping) -> requires a token
        r = srv.req({"operation": "stats"})
        check(r.get("ok") is False and r["error"]["code"] == "UNAUTHORIZED",
              "stats without token -> UNAUTHORIZED")
        r = srv.req({"operation": "stats", "token": "s3cret"})
        check(r.get("ok") is True, "stats with correct token -> ok")


def test_multitenancy(binary, port):
    print("[multi-tenant: namespace + scope]")
    lines = [
        "admintok",          # bare line -> global admin
        "acme_rw acme rw",   # namespaced read+write
        "acme_ro acme ro",   # namespaced read-only
        "beta_rw beta rw",   # a different tenant
    ]
    with Server(binary, port, phase=4, token_lines=lines) as srv:
        # a namespaced write is pinned to the token's namespace, even if the
        # client asks for a different agent_id
        r = srv.req({"operation": "insert", "type": "episodic",
                     "data": "acme secret", "agent_id": "beta", "token": "acme_rw"})
        check(r.get("ok") is True and r["record"].get("agent_id") == "acme",
              "namespaced insert is pinned to its namespace")
        acme_id = r["record"]["id"]

        # the owning tenant can read its own record
        r = srv.req({"operation": "get", "id": acme_id, "token": "acme_rw"})
        check(r.get("ok") is True and r["record"]["data"] == "acme secret",
              "owner reads its own record")

        # another tenant cannot see it (NOT_FOUND, not UNAUTHORIZED -> no leak)
        r = srv.req({"operation": "get", "id": acme_id, "token": "beta_rw"})
        check(r.get("ok") is False and r["error"]["code"] == "NOT_FOUND",
              "cross-tenant get -> NOT_FOUND")

        # another tenant cannot delete it either
        r = srv.req({"operation": "delete", "id": acme_id, "token": "beta_rw"})
        check(r.get("ok") is False and r["error"]["code"] == "NOT_FOUND",
              "cross-tenant delete -> NOT_FOUND")

        # search is scoped to the caller's namespace
        srv.req({"operation": "insert", "type": "episodic", "data": "beta note",
                 "token": "beta_rw"})
        r = srv.req({"operation": "search", "top_k": 100, "token": "beta_rw"})
        agents = {rec.get("agent_id") for rec in r.get("records", [])}
        check(r.get("ok") is True and agents == {"beta"},
              "search returns only the caller's namespace")

        # a read-only token cannot write
        r = srv.req({"operation": "insert", "type": "episodic",
                     "data": "nope", "token": "acme_ro"})
        check(r.get("ok") is False and r["error"]["code"] == "FORBIDDEN",
              "read-only token write -> FORBIDDEN")

        # ...but can read within its namespace
        r = srv.req({"operation": "get", "id": acme_id, "token": "acme_ro"})
        check(r.get("ok") is True, "read-only token can read its namespace")

        # an admin token sees across namespaces
        r = srv.req({"operation": "get", "id": acme_id, "token": "admintok"})
        check(r.get("ok") is True, "admin token reads any namespace")

        # working memory is namespace-scoped too: another tenant cannot promote
        # a working record even with the right session_id (#17).
        r = srv.req({"operation": "insert", "type": "working",
                     "session_id": "s-acme", "data": "wm-secret", "token": "acme_rw"})
        check(r.get("ok") is True, "acme inserts a working record")
        wid = r["record"]["id"]
        r = srv.req({"operation": "promote", "session_id": "s-acme",
                     "working_id": wid, "to_type": "semantic", "token": "beta_rw"})
        check(r.get("ok") is False and r["error"]["code"] == "NOT_FOUND",
              "cross-tenant promote -> NOT_FOUND")
        r = srv.req({"operation": "promote", "session_id": "s-acme",
                     "working_id": wid, "to_type": "semantic", "token": "acme_rw"})
        check(r.get("ok") is True and r["record"].get("agent_id") == "acme",
              "owner promotes its own working record")

        # stats is admin-only
        check(srv.req({"operation": "stats", "token": "admintok"}).get("ok") is True,
              "admin token may call stats")
        r = srv.req({"operation": "stats", "token": "acme_rw"})
        check(r.get("ok") is False and r["error"]["code"] == "FORBIDDEN",
              "namespaced token stats -> FORBIDDEN")

        # missing / wrong tokens are rejected
        r = srv.req({"operation": "get", "id": acme_id})
        check(r.get("ok") is False and r["error"]["code"] == "UNAUTHORIZED",
              "no token -> UNAUTHORIZED")
        r = srv.req({"operation": "get", "id": acme_id, "token": "bogus"})
        check(r.get("ok") is False and r["error"]["code"] == "UNAUTHORIZED",
              "unknown token -> UNAUTHORIZED")


def test_hashed_tokens(binary, port):
    print("[auth: tokens hashed at rest]")
    secret = "acme-secret-token"
    digest = hashlib.sha256(secret.encode()).hexdigest()

    # --hash-token must produce the same 'sha256$<hex>' the server accepts
    out = subprocess.run([binary, "--hash-token", secret],
                         capture_output=True, text=True).stdout.strip()
    check(out == "sha256$" + digest, "--hash-token matches sha256(token)")

    lines = ["admintok", f"sha256${digest} acme rw"]
    with Server(binary, port, phase=4, token_lines=lines) as srv:
        # the plaintext token authenticates against the stored hash
        r = srv.req({"operation": "insert", "type": "episodic",
                     "data": "hashed-auth", "token": secret})
        check(r.get("ok") is True and r["record"].get("agent_id") == "acme",
              "plaintext token authenticates against hashed entry")
        # and still carries its namespace + scope
        r = srv.req({"operation": "search", "top_k": 10, "token": secret})
        check(r.get("ok") is True, "hashed token retains read access")
        # a wrong token is rejected
        r = srv.req({"operation": "insert", "type": "episodic",
                     "data": "x", "token": "wrong"})
        check(r.get("ok") is False and r["error"]["code"] == "UNAUTHORIZED",
              "wrong token against hashed entry -> UNAUTHORIZED")


def _cli(binary, *args, token=None):
    env = dict(os.environ)
    env.pop("AEGIS_TOKEN", None)
    if token:
        env["AEGIS_TOKEN"] = token
    return subprocess.run([binary, "client", *args], capture_output=True,
                          text=True, env=env)


def test_cli(binary, port):
    print("[client CLI + gen-token]")
    with Server(binary, port, phase=4) as srv:
        p = str(port)
        r = _cli(binary, "--port", p, "ping")
        check(r.returncode == 0 and json.loads(r.stdout).get("ok") is True,
              "client ping -> ok, exit 0")

        r = _cli(binary, "--port", p, "put", "--type", "semantic",
                 "--tags", "user,pref", "likes dark mode")
        check(r.returncode == 0, "client put -> exit 0")
        rid = json.loads(r.stdout)["record"]["id"]

        r = _cli(binary, "--port", p, "get", str(rid))
        check(r.returncode == 0
              and json.loads(r.stdout)["record"]["data"] == "likes dark mode",
              "client get returns the record")

        r = _cli(binary, "--port", p, "get", "999999")
        check(r.returncode == 1, "client get missing -> exit 1")

        r = _cli(binary, "--port", p, "search", "--tags", "user")
        check(r.returncode == 0 and json.loads(r.stdout)["total"] >= 1,
              "client search finds the record")

        # --query is the CLI path to lexical search (ROADMAP 4.1); without it the
        # feature would only be reachable by hand-writing JSON.
        r = _cli(binary, "--port", p, "search", "--query", "dark mode")
        check(r.returncode == 0
              and [m["id"] for m in json.loads(r.stdout)["records"]] == [rid],
              "client search --query finds the record by keyword")
        r = _cli(binary, "--port", p, "search", "--query", "zzz_absent")
        check(r.returncode == 0 and json.loads(r.stdout)["total"] == 0,
              "client search --query with no match -> empty, exit 0")

    # gen-token -> token-file line + plaintext token that authenticates
    g = subprocess.run([binary, "gen-token", "--namespace", "acme", "--scope",
                        "rw"], capture_output=True, text=True)
    line = next((l for l in g.stdout.splitlines() if l.startswith("sha256$")), None)
    tok = next((l[len("token: "):] for l in g.stdout.splitlines()
                if l.startswith("token: ")), None)
    check(line is not None and tok, "gen-token prints a file line and a token")

    with Server(binary, port + 1, phase=4, token_lines=[line]):
        p = str(port + 1)
        r = _cli(binary, "--port", p, "put", "--tags", "t", "hello", token=tok)
        check(r.returncode == 0
              and json.loads(r.stdout)["record"]["agent_id"] == "acme",
              "gen-token token authenticates and pins its namespace")
        r = _cli(binary, "--port", p, "put", "nope")  # no token
        check(r.returncode == 1, "client without a token -> exit 1")

    # gen-key mints a 32-byte (64 hex char) encryption key on stdout
    g = subprocess.run([binary, "gen-key"], capture_output=True, text=True)
    key = g.stdout.strip()
    check(g.returncode == 0 and len(key) == 64
          and all(c in "0123456789abcdef" for c in key),
          "gen-key prints 64 hex chars")


def test_bulk_ops(binary, port):
    print("[bulk ops: batch insert, count, delete-by-query]")
    with Server(binary, port, phase=4) as srv:
        # batch insert: one request, several records
        r = srv.req({"operation": "insert", "records": [
            {"type": "episodic", "tags": ["bulk", "a"], "data": "one"},
            {"type": "episodic", "tags": ["bulk", "b"], "data": "two"},
            {"type": "episodic", "tags": ["bulk", "a"], "data": "three"},
        ]})
        check(r.get("ok") is True and r.get("count") == 3
              and len(r.get("records", [])) == 3, "batch insert -> 3 records")

        # a malformed element rejects the whole batch (nothing written)
        r = srv.req({"operation": "insert", "records": [
            {"type": "episodic", "data": "ok"},
            {"type": "episodic"},  # missing data
        ]})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "batch with a bad element -> INVALID_REQUEST")

        # count by filter
        r = srv.req({"operation": "count", "tags": ["bulk"]})
        check(r.get("ok") is True and r.get("count") == 3,
              "count tag=bulk -> 3 (batch write landed, bad batch did not)")
        r = srv.req({"operation": "count", "tags": ["a"]})
        check(r.get("ok") is True and r.get("count") == 2, "count tag=a -> 2")

        # delete-by-query requires at least one filter
        r = srv.req({"operation": "delete"})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "unfiltered delete-by-query -> INVALID_REQUEST")

        # delete the 'a' subset, leaving the 'b' one
        r = srv.req({"operation": "delete", "tags": ["a"]})
        check(r.get("ok") is True and r.get("deleted") == 2,
              "delete tag=a -> deleted 2")
        r = srv.req({"operation": "count", "tags": ["bulk"]})
        check(r.get("ok") is True and r.get("count") == 1, "count bulk -> 1 remains")


def test_traverse_kinds(binary, port):
    print("[traverse: edge-kind filter + per-hop attribution (ROADMAP 5.1)]")
    with Server(binary, port, phase=4) as srv:
        def ins(data):
            return srv.req({"operation": "insert", "type": "semantic",
                            "data": data})["record"]["id"]

        def rel(a, b, kind=None):
            p = {"operation": "relate", "from_id": a, "to_id": b}
            if kind is not None:
                p["kind"] = kind
            check(srv.req(p).get("ok") is True, f"relate {a}->{b} ({kind})")

        def walk(start, **kw):
            p = {"operation": "traverse", "id": start, "include_embeddings": False}
            p.update(kw)
            return srv.req(p)

        # a -derived_from-> b -derived_from-> d,  a -supersedes-> c
        a, b, c, d = ins("a"), ins("b"), ins("c"), ins("d")
        rel(a, b, "derived_from")
        rel(a, c, "supersedes")
        rel(b, d, "derived_from")

        # unfiltered walk is unchanged by 5.1: every kind is followed
        r = walk(a, depth=2)
        ids = sorted(rec["id"] for rec in r.get("records", []))
        check(r.get("ok") is True and ids == sorted([a, b, c, d]),
              "unfiltered traverse still follows every edge kind")

        # the filter is the point: `supersedes` is not followed
        r = walk(a, depth=2, kinds=["derived_from"])
        ids = sorted(rec["id"] for rec in r.get("records", []))
        check(r.get("ok") is True and ids == sorted([a, b, d]),
              "kinds filter excludes an unrequested edge kind")

        # ...and the converse, one hop of the other kind only
        r = walk(a, depth=2, kinds=["supersedes"])
        ids = sorted(rec["id"] for rec in r.get("records", []))
        check(r.get("ok") is True and ids == sorted([a, c]),
              "kinds filter follows only the requested kind")

        # several kinds at once = union
        r = walk(a, depth=1, kinds=["supersedes", "derived_from"])
        ids = sorted(rec["id"] for rec in r.get("records", []))
        check(r.get("ok") is True and ids == sorted([a, b, c]),
              "multiple kinds are a union")

        # a kind nobody wrote reaches only the start record
        r = walk(a, depth=2, kinds=["no_such_kind"])
        ids = [rec["id"] for rec in r.get("records", [])]
        check(r.get("ok") is True and ids == [a],
              "unknown kind yields just the start record")

        # per-hop attribution: every non-start hop names the edge that reached it
        r = walk(a, depth=2, kinds=["derived_from"])
        hop = {rec["id"]: rec.get("traversal") for rec in r["records"]}
        check(hop[a] == {"depth": 0},
              "start record reports depth 0 and no reaching edge")
        check(hop[b] == {"depth": 1, "via_id": a, "via_kind": "derived_from",
                         "via_direction": "out"},
              "depth-1 hop names its reaching edge")
        check(hop[d] == {"depth": 2, "via_id": b, "via_kind": "derived_from",
                         "via_direction": "out"},
              "depth-2 hop names the edge from its actual parent")

        # an unkinded edge is followed only when no kinds are named
        e, f = ins("e"), ins("f")
        rel(e, f)  # no kind
        r = walk(e, depth=1)
        ids = sorted(rec["id"] for rec in r.get("records", []))
        check(r.get("ok") is True and ids == sorted([e, f]),
              "unkinded edge followed by an unfiltered walk")
        r = walk(e, depth=1, kinds=["derived_from"])
        ids = [rec["id"] for rec in r.get("records", [])]
        check(r.get("ok") is True and ids == [e],
              "unkinded edge not followed once kinds are named")
        # ...and it reports no via_kind rather than an empty one
        r = walk(e, depth=1)
        hop = {rec["id"]: rec.get("traversal") for rec in r["records"]}
        check(hop[f] == {"depth": 1, "via_id": e, "via_direction": "out"},
              "unkinded edge reports via_id with no via_kind")

        # direction: the three real values are accepted, anything else is not
        for good in ("out", "in", "both"):
            r = walk(a, depth=1, direction=good)
            check(r.get("ok") is True, f'direction "{good}" is accepted')
        for bad in ("sideways", "OUT", ""):
            r = walk(a, depth=1, direction=bad)
            check(r.get("ok") is False
                  and r["error"]["code"] == "INVALID_REQUEST",
                  f'direction "{bad}" rejected, not silently ignored')

        # A `kind` longer than the reverse index can intern is refused rather
        # than accepted-and-unlabelled: an un-internable kind would degrade a
        # filtered reverse walk into a candidate set nobody asked for.
        r = srv.req({"operation": "relate", "from_id": a, "to_id": c,
                     "kind": "k" * 65})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "an over-long edge kind is rejected")
        r = srv.req({"operation": "relate", "from_id": a, "to_id": c,
                     "kind": "k" * 64})
        check(r.get("ok") is True, "a kind at exactly the limit is accepted")

        # A malformed `kinds` must not silently become an unfiltered walk — the
        # same strictness `direction` gets. Asking to narrow and getting the
        # widest possible result is the failure worth refusing.
        for bad in ([123], ["derived_from", 5], "derived_from", {"a": 1}):
            r = walk(a, depth=1, kinds=bad)
            check(r.get("ok") is False
                  and r["error"]["code"] == "INVALID_REQUEST",
                  f"malformed kinds {bad!r} rejected, not silently widened")
        r = walk(a, depth=1, kinds=[])
        check(r.get("ok") is True, "an explicitly empty kinds list is allowed")

        # the kinds list is capped (MAX_TRAVERSE_KINDS = 16)
        r = walk(a, depth=1, kinds=[f"k{i}" for i in range(16)])
        check(r.get("ok") is True, "16 kinds accepted")
        r = walk(a, depth=1, kinds=[f"k{i}" for i in range(17)])
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "17 kinds rejected")


def test_edge_index_maintenance(binary, port):
    print("[edge index: maintained on write, rebuilt on restart (ROADMAP 5.1)]")
    datadir = tempfile.mkdtemp(prefix="aegis_edgeidx_")

    def edges(srv):
        st = srv.req({"operation": "stats"})
        return st["indexes"]["edges"], st["indexes"]["edge_kinds"]

    with Server(binary, port, phase=4, datadir=datadir) as srv:
        def ins(d):
            return srv.req({"operation": "insert", "type": "semantic",
                            "data": d})["record"]["id"]

        check(edges(srv) == (0, 0), "fresh server indexes no edges")

        a, b, c = ins("a"), ins("b"), ins("c")
        check(edges(srv) == (0, 0), "insert alone creates no edges")

        # relate is the only op that creates an edge
        srv.req({"operation": "relate", "from_id": a, "to_id": b,
                 "kind": "supersedes"})
        check(edges(srv) == (1, 1), "relate indexes one edge, one kind")
        srv.req({"operation": "relate", "from_id": c, "to_id": b,
                 "kind": "derived_from"})
        check(edges(srv) == (2, 2), "second relate, second kind")

        # idempotent on the wire as well as in the index
        srv.req({"operation": "relate", "from_id": a, "to_id": b,
                 "kind": "supersedes"})
        check(edges(srv) == (2, 2), "re-relating the same edge does not double it")

        # an update touches tags/payload only, never the edge map
        srv.req({"operation": "update", "id": a, "data": "a-v2"})
        check(edges(srv) == (2, 2), "update leaves the edge index alone")

        # deleting a *target* drops its whole indegree (both edges point at b),
        # and releases the kinds those edges were the last users of — the count
        # tracks kinds in use, not kinds ever seen, so it survives a restart
        srv.req({"operation": "delete", "id": b})
        check(edges(srv) == (0, 0),
              "deleting a target drops every edge into it, and its kinds")

        # ...and deleting a *source* drops its outgoing edges
        d, e = ins("d"), ins("e")
        srv.req({"operation": "relate", "from_id": d, "to_id": e,
                 "kind": "mentions"})
        check(edges(srv)[0] == 1, "edge into e indexed")
        srv.req({"operation": "delete", "id": d})
        check(edges(srv)[0] == 0, "deleting the source drops its outgoing edge")

        # a surviving edge to carry across the restart
        f, g = ins("f"), ins("g")
        srv.req({"operation": "relate", "from_id": f, "to_id": g,
                 "kind": "derived_from"})
        before = edges(srv)
        check(before[0] == 1, "one live edge before restart")
        srv.graceful_stop()

    # The index is derived and never checkpointed, so recovery rebuilds it from
    # the log. Crucially it must NOT resurrect edges whose endpoint was deleted:
    # a restarted server has to report the same count as the one that wrote the
    # log, or the two disagree about what the graph contains.
    with Server(binary, port, phase=4, datadir=datadir) as srv:
        check(edges(srv) == before,
              f"restart rebuilds the same edge count ({before[0]})")

    # --no-edge-index: no reverse index, and the forward walk is untouched
    d2 = tempfile.mkdtemp(prefix="aegis_edgeoff_")
    with Server(binary, port, phase=4, datadir=d2,
                extra_args=["--no-edge-index"]) as srv:
        a = srv.req({"operation": "insert", "type": "semantic",
                     "data": "a"})["record"]["id"]
        b = srv.req({"operation": "insert", "type": "semantic",
                     "data": "b"})["record"]["id"]
        srv.req({"operation": "relate", "from_id": a, "to_id": b, "kind": "k"})
        st = srv.req({"operation": "stats"})
        check(st["indexes"]["edges"] == 0 and st["indexes"]["edge_kinds"] == 0,
              "--no-edge-index indexes nothing")
        check(st["memory"]["edge_bytes"] == 0,
              "--no-edge-index costs no edge RAM")
        r = srv.req({"operation": "traverse", "id": a, "depth": 1,
                     "kinds": ["k"]})
        check([rec["id"] for rec in r.get("records", [])] == [a, b],
              "forward traverse unaffected by --no-edge-index")


def test_edge_index_replica_parity(binary, port):
    print("[edge index: a replica agrees with its primary]")
    repl_port = port + 1
    replica_port = port + 2
    tok = "repl-secret"
    with Server(binary, port, extra_args=[
            "--replication-port", str(repl_port),
            "--replication-token", tok]) as primary:
        with Server(binary, replica_port, extra_args=[
                "--replicate-from", f"127.0.0.1:{repl_port}",
                "--replication-token", tok]) as replica:

            def edge_count(srv):
                return srv.req({"operation": "stats"})["indexes"]["edges"]

            def wait_edges(want, tries=60):
                for _ in range(tries):
                    if edge_count(replica) == want:
                        return True
                    time.sleep(0.1)
                return edge_count(replica) == want

            def ins(d):
                return primary.req({"operation": "insert", "type": "semantic",
                                    "data": d})["record"]["id"]

            a, b, c = ins("a"), ins("b"), ins("c")
            primary.req({"operation": "relate", "from_id": a, "to_id": b,
                         "kind": "supersedes"})
            primary.req({"operation": "relate", "from_id": c, "to_id": b,
                         "kind": "derived_from"})
            check(edge_count(primary) == 2, "primary indexed both edges")
            check(wait_edges(2), "replica replicated both edges")

            # A tombstone must drop the *incoming* edges on the replica too.
            # Replication ships whole records, so the replica only ever sees the
            # deleted version of b — it has to infer the indegree cleanup that
            # qe_delete does directly on the primary.
            primary.req({"operation": "delete", "id": b})
            check(edge_count(primary) == 0, "primary dropped b's indegree")
            check(wait_edges(0),
                  "replica dropped b's indegree on the tombstone")

            # A record's relationships array outlives its targets: a tombstone
            # never rewrites its peers, so `a` still names the deleted `b`.
            # Relating a again re-ships the whole record, and a replica that
            # re-indexes it blindly resurrects a->b — permanently, since nothing
            # ever revisits it. This is the case the tombstone check above does
            # NOT cover, and it diverged before the liveness guard was added.
            d = ins("d")
            primary.req({"operation": "relate", "from_id": a, "to_id": d,
                         "kind": "mentions"})
            check(edge_count(primary) == 1,
                  "primary indexed only the edge to the live target")
            check(wait_edges(1),
                  "replica did not resurrect the edge to the deleted target")


def test_traverse_reverse(binary, port):
    print("[traverse: walking edges backwards (ROADMAP 5.1)]")
    with Server(binary, port, phase=4) as srv:
        def ins(d):
            return srv.req({"operation": "insert", "type": "semantic",
                            "data": d})["record"]["id"]

        def rel(a, b, kind=None):
            p = {"operation": "relate", "from_id": a, "to_id": b}
            if kind is not None:
                p["kind"] = kind
            srv.req(p)

        def walk(start, **kw):
            p = {"operation": "traverse", "id": start,
                 "include_embeddings": False}
            p.update(kw)
            return srv.req(p)

        def ids(r):
            return sorted(rec["id"] for rec in r.get("records", []))

        # THE motivating case: a supersession chain. v3 supersedes v2 supersedes
        # v1, so the edges point newest -> oldest. "What superseded v1?" is a
        # question only a backward walk can answer.
        v1, v2, v3 = ins("v1"), ins("v2"), ins("v3")
        rel(v2, v1, "supersedes")
        rel(v3, v2, "supersedes")

        r = walk(v1, depth=2, direction="in", kinds=["supersedes"])
        check(r.get("ok") is True and ids(r) == sorted([v1, v2, v3]),
              "supersession chain retrieved backwards in one call")
        hop = {rec["id"]: rec.get("traversal") for rec in r["records"]}
        check(hop[v1] == {"depth": 0}, "start of a reverse walk has no edge")
        check(hop[v2] == {"depth": 1, "via_id": v1, "via_kind": "supersedes",
                          "via_direction": "in"},
              "reverse hop reports via_direction in")
        check(hop[v3]["depth"] == 2 and hop[v3]["via_id"] == v2,
              "reverse walk continues past one hop")

        # forward from the same node finds nothing: v1 points at no one
        r = walk(v1, depth=2, direction="out")
        check(ids(r) == [v1], "forward from the chain tail reaches only itself")
        # ...and forward from the head walks the other way
        r = walk(v3, depth=2, direction="out")
        check(ids(r) == sorted([v1, v2, v3]), "forward from the head still works")

        # the kind filter applies to the reverse direction too
        other = ins("other")
        rel(other, v1, "mentions")
        r = walk(v1, depth=1, direction="in", kinds=["supersedes"])
        check(ids(r) == sorted([v1, v2]), "reverse walk honours the kind filter")
        r = walk(v1, depth=1, direction="in")
        check(ids(r) == sorted([v1, v2, other]), "unfiltered reverse finds both")
        r = walk(v1, depth=1, direction="in", kinds=["no_such_kind"])
        check(ids(r) == [v1], "reverse walk with an unknown kind finds nothing")

        # both: a node in the middle of the chain sees each side, and each hop
        # says which way it was reached
        r = walk(v2, depth=1, direction="both")
        check(ids(r) == sorted([v1, v2, v3]), "both directions from the middle")
        hop = {rec["id"]: rec.get("traversal") for rec in r["records"]}
        check(hop[v1]["via_direction"] == "out",
              "the node v2 points at is reached outward")
        check(hop[v3]["via_direction"] == "in",
              "the node pointing at v2 is reached inward")

        # an unkinded edge is followed by an unfiltered reverse walk only
        u1, u2 = ins("u1"), ins("u2")
        rel(u1, u2)
        r = walk(u2, depth=1, direction="in")
        check(ids(r) == sorted([u1, u2]), "unkinded edge walked backwards")
        r = walk(u2, depth=1, direction="in", kinds=["mentions"])
        check(ids(r) == [u2], "named filter does not match an unkinded edge")

        # a cycle must terminate rather than revisit
        c1, c2 = ins("c1"), ins("c2")
        rel(c1, c2, "loops")
        rel(c2, c1, "loops")
        r = walk(c1, depth=5, direction="both")
        check(r.get("ok") is True and ids(r) == sorted([c1, c2]),
              "a cycle terminates without repeating records")


def test_traverse_reverse_disabled(binary, port):
    print("[traverse: reverse needs the edge index]")
    with Server(binary, port, phase=4,
                extra_args=["--no-edge-index"]) as srv:
        a = srv.req({"operation": "insert", "type": "semantic",
                     "data": "a"})["record"]["id"]
        b = srv.req({"operation": "insert", "type": "semantic",
                     "data": "b"})["record"]["id"]
        srv.req({"operation": "relate", "from_id": a, "to_id": b, "kind": "k"})
        for d in ("in", "both"):
            r = srv.req({"operation": "traverse", "id": b, "direction": d})
            check(r.get("ok") is False and r["error"]["code"] == "NOT_READY",
                  f'direction "{d}" -> NOT_READY without the edge index')
        r = srv.req({"operation": "traverse", "id": a, "direction": "out"})
        check(r.get("ok") is True,
              "the forward walk needs no index and still works")


def test_traverse_reverse_recovery(binary, port):
    print("[traverse: reverse walk survives a restart]")
    datadir = tempfile.mkdtemp(prefix="aegis_revrec_")
    with Server(binary, port, phase=4, datadir=datadir) as srv:
        def ins(d):
            return srv.req({"operation": "insert", "type": "semantic",
                            "data": d})["record"]["id"]
        keep_src, target = ins("keeps pointing"), ins("target")
        gone_src = ins("will be deleted")
        srv.req({"operation": "relate", "from_id": keep_src,
                 "to_id": target, "kind": "supersedes"})
        srv.req({"operation": "relate", "from_id": gone_src,
                 "to_id": target, "kind": "supersedes"})
        srv.req({"operation": "delete", "id": gone_src})
        srv.graceful_stop()

    with Server(binary, port, phase=4, datadir=datadir) as srv:
        r = srv.req({"operation": "traverse", "id": target, "depth": 1,
                     "direction": "in", "include_embeddings": False})
        got = sorted(rec["id"] for rec in r.get("records", []))
        check(got == sorted([target, keep_src]),
              "reverse walk answers identically after a restart")
        check(gone_src not in got,
              "a deleted source is not resurrected by the rebuild")


def test_traverse_reverse_isolation(binary, port):
    print("[traverse: a reverse walk cannot cross a tenant boundary]")
    tokens = ["acme-key   acme   rw", "beta-key   beta   rw", "admin-key admin"]
    with Server(binary, port, phase=4, token_lines=tokens) as srv:
        def ins(tok, d):
            return srv.req({"operation": "insert", "type": "semantic",
                            "data": d, "token": tok})["record"]["id"]

        # acme owns the target; beta owns a record pointing AT it. Only an admin
        # can relate across namespaces, which is exactly how the interesting
        # situation arises: acme's record now has an incoming edge it does not
        # own, sitting in the reverse index under acme's own id.
        target = ins("acme-key", "acme target")
        beta_src = ins("beta-key", "beta source")
        acme_src = ins("acme-key", "acme source")
        for src in (beta_src, acme_src):
            r = srv.req({"operation": "relate", "from_id": src,
                         "to_id": target, "kind": "mentions",
                         "token": "admin-key"})
            check(r.get("ok") is True, f"admin related {src} -> {target}")

        # An admin sees the whole picture...
        r = srv.req({"operation": "traverse", "id": target, "depth": 1,
                     "direction": "in", "token": "admin-key",
                     "include_embeddings": False})
        check(sorted(rec["id"] for rec in r.get("records", []))
              == sorted([target, beta_src, acme_src]),
              "admin sees both incoming sources")

        # ...but acme walking backwards from its OWN record must not learn that
        # beta's record points at it. This is the leak a reverse index makes
        # newly possible, and the namespace filter has to catch it.
        r = srv.req({"operation": "traverse", "id": target, "depth": 1,
                     "direction": "in", "token": "acme-key",
                     "include_embeddings": False})
        got = sorted(rec["id"] for rec in r.get("records", []))
        check(got == sorted([target, acme_src]),
              "acme sees only its own incoming source")
        check(beta_src not in got,
              "a co-tenant's incoming edge is not revealed by a reverse walk")

        # and beta cannot use acme's id as a starting point at all
        r = srv.req({"operation": "traverse", "id": target, "depth": 1,
                     "direction": "in", "token": "beta-key"})
        check(r.get("ok") is True and r.get("records") == [],
              "a foreign start record yields nothing, not a leak")


REPL_MAGIC = 0xA6E515ED
MSG_FRAME, MSG_HEARTBEAT, MSG_RESET, MSG_INCOMPATIBLE = 0, 1, 2, 3


def _fake_replica(repl_port, token, codec_version, want=2, timeout=5.0,
                  from_offset=0, generation=0):
    """Subscribe to a primary's replication stream as a bare socket and return
    (handshake_response, [message types seen]).

    A real replica cannot be made to declare an old codec version — the number
    is compiled in — so the gate is driven from a hand-rolled peer instead.
    Reads up to `want` messages or until the primary hangs up.
    """
    s = socket.create_connection(("127.0.0.1", repl_port), timeout)
    s.settimeout(timeout)
    hs = {"from_offset": from_offset, "generation": generation,
          "token": token, "key_fingerprint": ""}
    if codec_version is not None:
        hs["codec_version"] = codec_version
    s.sendall((json.dumps(hs) + "\n").encode())

    # Read the handshake line byte at a time, in binary. A buffered text-mode
    # makefile() would read ahead past the newline into the binary frames that
    # follow immediately — and those contain both 0x0a and invalid UTF-8, so it
    # would either swallow a frame or fail to decode. (It did, intermittently,
    # depending on how quickly the primary started streaming.)
    line = b""
    while not line.endswith(b"\n"):
        ch = s.recv(1)
        if not ch:
            break
        line += ch
    resp = json.loads(line.decode())

    types = []
    try:
        while len(types) < want:
            hdr = b""
            while len(hdr) < 17:
                chunk = s.recv(17 - len(hdr))
                if not chunk:
                    raise EOFError
                hdr += chunk
            magic = int.from_bytes(hdr[0:4], "little")
            if magic != REPL_MAGIC:
                raise AssertionError(f"bad magic {magic:#x}")
            mtype = hdr[4]
            length = int.from_bytes(hdr[13:17], "little")
            body = b""
            while len(body) < length:
                chunk = s.recv(length - len(body))
                if not chunk:
                    raise EOFError
                body += chunk
            types.append((mtype, body))
    except (EOFError, socket.timeout, OSError):
        pass
    s.close()
    return resp, types


def test_replication_codec_gate(binary, port):
    print("[replication: refuse a frame the replica cannot decode]")
    repl_port = port + 1
    tok = "repl-secret"
    with Server(binary, port, extra_args=[
            "--replication-port", str(repl_port),
            "--replication-token", tok]) as primary:
        primary.req({"operation": "insert", "type": "semantic",
                     "data": "a record to ship"})

        # A peer that can read what this build writes gets the frame.
        resp, msgs = _fake_replica(repl_port, tok, codec_version=3, want=1)
        check(resp.get("ok") is True, "codec-aware replica is accepted")
        kinds = [m[0] for m in msgs]
        check(MSG_FRAME in kinds, "a readable frame is streamed")
        body = [m[1] for m in msgs if m[0] == MSG_FRAME][0]
        check(body[0] == 2,
              "a fact-less record still ships as codec v2")

        # A peer that predates the field is assumed to top out at v2, which is
        # what every fact-less frame is — so nothing changes for it. This is the
        # case a handshake-time version check would have broken.
        resp, msgs = _fake_replica(repl_port, tok, codec_version=None, want=1)
        check(resp.get("ok") is True, "replica omitting codec_version accepted")
        check(MSG_FRAME in [m[0] for m in msgs],
              "an old replica still receives v2 frames")

        # A peer that cannot read v2 must be told, not fed the frame. (Real
        # builds all read v2; declaring v1 is how the gate's comparison is
        # exercised without a v3 frame, which nothing can write yet.)
        resp, msgs = _fake_replica(repl_port, tok, codec_version=1, want=1)
        check(resp.get("ok") is True, "subscription itself still succeeds")
        kinds = [m[0] for m in msgs]
        check(MSG_INCOMPATIBLE in kinds,
              "an unreadable frame yields MSG_INCOMPATIBLE, not the frame")
        check(MSG_FRAME not in kinds,
              "the undecodable frame is never sent")

        # from_offset/generation cannot be converted from a negative or
        # >= 2^64 double, so a peer sending one is refused rather than seeked
        # to an arbitrary place in the log — or silently re-sent all of it.
        # Rejected after the token check, so it is not an oracle for anything.
        for field in ("from_offset", "generation"):
            for bad in (-1, 1e30):
                resp, msgs = _fake_replica(repl_port, tok, codec_version=3,
                                           want=1, **{field: bad})
                check(resp.get("ok") is False,
                      f"{field}={bad!r} is refused, not converted")
                check(MSG_FRAME not in [m[0] for m in msgs],
                      f"and nothing is streamed for {field}={bad!r}")
        resp, _ = _fake_replica(repl_port, "wrong-token", codec_version=3,
                                want=1, from_offset=-1)
        check(resp.get("ok") is False and "malformed" not in str(resp),
              "a bad token outranks a malformed offset, so it leaks nothing")

        # a valid offset still subscribes: the guard rejects only what cannot
        # be an offset, not an unusual one
        resp, _ = _fake_replica(repl_port, tok, codec_version=3, want=1,
                                from_offset=0, generation=1)
        check(resp.get("ok") is True, "a well-formed handshake still works")


def test_typed_facts(binary, port):
    print("[typed facts: stored, echoed, indexed, rebuilt (ROADMAP 5.2)]")
    datadir = tempfile.mkdtemp(prefix="aegis_facts_")

    def stats_facts(srv):
        st = srv.req({"operation": "stats"})
        return st["indexes"]["facts"], st["indexes"]["fact_predicates"]

    with Server(binary, port, phase=4, datadir=datadir) as srv:
        def ins(data, fact=None, **kw):
            p = {"operation": "insert", "type": "semantic", "data": data}
            if fact is not None:
                p["fact"] = fact
            p.update(kw)
            return srv.req(p)

        check(stats_facts(srv) == (0, 0), "a fresh server indexes no facts")

        # entity records: the convention a fact's subject refers to
        hook = ins("the recall hook")["record"]["id"]
        storage = ins("the storage layer")["record"]["id"]

        # a literal-object fact round-trips through the response
        r = ins("The recall hook defaults to embedding_mode=none.",
                {"s": hook, "p": "defaults_to", "o": "none"})
        check(r.get("ok") is True, "insert with a literal-object fact")
        f = r["record"].get("fact")
        check(f == {"s": hook, "p": "defaults_to", "o": "none"},
              "the fact is echoed as written")
        lit = r["record"]["id"]

        # an id-object fact uses {"id": N} so it cannot be confused with a
        # literal that happens to look like a number
        r = ins("hnsw.c is part of the storage layer",
                {"s": hook, "p": "part_of", "o": {"id": storage}})
        check(r["record"].get("fact") == {"s": hook, "p": "part_of",
                                          "o": {"id": storage}},
              "an id-valued object round-trips as {id: N}")
        idf = r["record"]["id"]

        check(stats_facts(srv) == (2, 2), "both facts indexed, two predicates")

        # get echoes it too, not just insert
        r = srv.req({"operation": "get", "id": lit})
        check(r["record"].get("fact", {}).get("p") == "defaults_to",
              "get echoes the fact")

        # a record with no fact has no fact key at all
        r = srv.req({"operation": "get", "id": hook})
        check("fact" not in r["record"],
              "a fact-less record carries no fact field")

        # --- validation ---
        bad = [
            ({"p": "x", "o": "y"}, "missing subject"),
            ({"s": hook, "o": "y"}, "missing predicate"),
            ({"s": hook, "p": "", "o": "y"}, "empty predicate"),
            ({"s": hook, "p": "x"}, "missing object"),
            ({"s": hook, "p": "x", "o": 42}, "a bare number is not an object"),
            ({"s": hook, "p": "x", "o": {"nope": 1}}, "an object without id"),
            ({"s": hook, "p": "k" * 65, "o": "y"}, "an over-long predicate"),
            ("not-an-object", "a non-object fact"),
        ]
        for fact, why in bad:
            r = ins("bad", fact)
            check(r.get("ok") is False
                  and r["error"]["code"] == "INVALID_REQUEST",
                  f"rejected: {why}")
        check(stats_facts(srv) == (2, 2), "no rejected fact was indexed")

        # a predicate at exactly the limit is fine
        r = ins("at the limit", {"s": hook, "p": "k" * 64, "o": "y"})
        check(r.get("ok") is True, "a 64-byte predicate is accepted")

        # --- delete unindexes ---
        srv.req({"operation": "delete", "id": idf})
        check(stats_facts(srv)[0] == 2, "deleting a record drops its fact")

        # --- update leaves the fact alone (it is immutable by design) ---
        srv.req({"operation": "update", "id": lit, "data": "reworded"})
        r = srv.req({"operation": "get", "id": lit})
        check(r["record"].get("fact", {}).get("o") == "none",
              "an update does not disturb the fact")
        check(stats_facts(srv)[0] == 2, "nor the fact index")

        # and an update that *tries* to set one is refused rather than silently
        # dropped — the caller spelled the field correctly and deserves to
        # learn that supersession, not editing, is how a claim changes
        r = srv.req({"operation": "update", "id": lit,
                     "fact": {"s": hook, "p": "defaults_to", "o": "other"}})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "update refuses a fact instead of ignoring it")
        r = srv.req({"operation": "get", "id": lit})
        check(r["record"].get("fact", {}).get("o") == "none",
              "and the original claim stands")
        r = srv.req({"operation": "update", "id": lit, "data": "again",
                     "fact": None})
        check(r.get("ok") is True, "an explicit null fact is not a change")

        # `derivation` is written by the inference job (ROADMAP 5.3) and by
        # nothing else. A client that could supply one could manufacture
        # provenance, so both write paths refuse it rather than dropping it.
        d = {"rule": "transitive", "depth": 1, "premises": [hook]}
        r = srv.req({"operation": "insert", "type": "semantic", "data": "forged",
                     "fact": {"s": hook, "p": "defaults_to", "o": "x"},
                     "derivation": d})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "insert refuses a client-supplied derivation")
        r = srv.req({"operation": "insert", "records": [
            {"type": "semantic", "data": "ok"},
            {"type": "semantic", "data": "forged", "derivation": d}]})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "and a batch element carrying one rejects the whole batch")
        r = srv.req({"operation": "update", "id": lit, "derivation": d})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "update refuses one too, the other way a client might author it")
        r = srv.req({"operation": "insert", "type": "semantic", "data": "fine",
                     "derivation": None})
        check(r.get("ok") is True, "an explicit null derivation is not one")

        before = stats_facts(srv)
        srv.graceful_stop()

    # derived and never checkpointed: recovery must rebuild all of it
    with Server(binary, port, phase=4, datadir=datadir) as srv:
        check(stats_facts(srv) == before,
              f"restart rebuilds the same facts {before}")
        r = srv.req({"operation": "get", "id": lit})
        check(r["record"].get("fact", {}).get("p") == "defaults_to",
              "the fact survives the restart durably, not just in the index")

    # --no-fact-index: the fact is still stored, just not indexed
    d2 = tempfile.mkdtemp(prefix="aegis_nofacts_")
    with Server(binary, port, phase=4, datadir=d2,
                extra_args=["--no-fact-index"]) as srv:
        e = srv.req({"operation": "insert", "type": "semantic",
                     "data": "e"})["record"]["id"]
        r = srv.req({"operation": "insert", "type": "semantic", "data": "f",
                     "fact": {"s": e, "p": "defaults_to", "o": "none"}})
        check(r.get("ok") is True, "--no-fact-index still accepts a fact")
        check(r["record"].get("fact", {}).get("o") == "none",
              "and still stores it: the record keeps what it asserts")
        st = srv.req({"operation": "stats"})
        check(st["indexes"]["facts"] == 0 and st["memory"]["fact_bytes"] == 0,
              "but indexes nothing and costs no fact RAM")


def test_codec_gate_with_a_real_v3_frame(binary, port):
    print("[replication: a real v3 frame is withheld from an older replica]")
    repl_port = port + 1
    tok = "repl-secret"
    with Server(binary, port, extra_args=[
            "--replication-port", str(repl_port),
            "--replication-token", tok]) as primary:
        subj = primary.req({"operation": "insert", "type": "semantic",
                            "data": "subject"})["record"]["id"]
        primary.req({"operation": "insert", "type": "semantic",
                     "data": "a fact", "fact": {
                         "s": subj, "p": "defaults_to", "o": "none"}})

        # PR 2 could only exercise the gate with a contrived codec_version of 1,
        # because nothing could write a v3 frame yet. Now one exists, so this is
        # the real case: a replica that reads up to v2 must get the v2 frames
        # and then be told, not handed the v3 one.
        _, msgs = _fake_replica(repl_port, tok, codec_version=2, want=5)
        kinds = [m[0] for m in msgs]
        versions = [m[1][0] for m in msgs if m[0] == MSG_FRAME and m[1]]
        check(2 in versions, "the fact-less frames still stream as v2")
        check(3 not in versions, "the v3 frame is never sent to a v2 replica")
        check(MSG_INCOMPATIBLE in kinds,
              "the replica is told why the stream stopped")

        # ...and a current replica receives it, v3 and all.
        _, msgs = _fake_replica(repl_port, tok, codec_version=3, want=5)
        versions = [m[1][0] for m in msgs if m[0] == MSG_FRAME and m[1]]
        check(3 in versions, "a v3-capable replica receives the v3 frame")
        check(MSG_INCOMPATIBLE not in [m[0] for m in msgs],
              "and is not refused")

        # A negative version cannot be converted to unsigned at all (that is
        # undefined), so it reads as "no field" — the v2 default, which is the
        # direction that withholds a frame rather than streaming one to a peer
        # that never claimed to understand it.
        _, msgs = _fake_replica(repl_port, tok, codec_version=-1, want=5)
        versions = [m[1][0] for m in msgs if m[0] == MSG_FRAME and m[1]]
        check(3 not in versions, "a negative codec_version defaults to v2")
        check(MSG_INCOMPATIBLE in [m[0] for m in msgs],
              "and the peer is still told why")

        # Anything at or above what this build can write is clamped to that:
        # a future replica reads everything we emit, and there is no principled
        # line above RECORD_CODEC_MAX that separates "newer build" from "absurd
        # number" — both can decode every frame this primary is able to produce.
        for high in (99, 1e30):
            _, msgs = _fake_replica(repl_port, tok, codec_version=high, want=5)
            versions = [m[1][0] for m in msgs if m[0] == MSG_FRAME and m[1]]
            check(3 in versions,
                  f"codec_version {high!r} is clamped to what we can write")


def test_typed_facts_replica_parity(binary, port):
    print("[typed facts: a replica indexes them identically]")
    repl_port = port + 1
    replica_port = port + 2
    tok = "repl-secret"
    with Server(binary, port, extra_args=[
            "--replication-port", str(repl_port),
            "--replication-token", tok]) as primary:
        with Server(binary, replica_port, extra_args=[
                "--replicate-from", f"127.0.0.1:{repl_port}",
                "--replication-token", tok]) as replica:

            def facts(srv):
                return srv.req({"operation": "stats"})["indexes"]["facts"]

            def wait_facts(want, tries=60):
                for _ in range(tries):
                    if facts(replica) == want:
                        return True
                    time.sleep(0.1)
                return facts(replica) == want

            subj = primary.req({"operation": "insert", "type": "semantic",
                                "data": "subject"})["record"]["id"]
            r = primary.req({"operation": "insert", "type": "semantic",
                             "data": "a fact", "fact": {
                                 "s": subj, "p": "defaults_to", "o": "none"}})
            fid = r["record"]["id"]
            check(facts(primary) == 1, "primary indexed the fact")
            check(wait_facts(1), "replica indexed the replicated fact")

            # This is also the first end-to-end exercise of the PR-2 codec gate:
            # the frame carrying a fact is codec v3, and a same-version replica
            # must accept it rather than be refused.
            r = replica.req({"operation": "get", "id": fid})
            check(r.get("record", {}).get("fact", {}).get("o") == "none",
                  "the fact itself replicated, not just the count")

            primary.req({"operation": "delete", "id": fid})
            check(facts(primary) == 0, "primary dropped it")
            check(wait_facts(0), "replica dropped it too")


def test_predicate_registry(binary, port):
    print("[predicate registry: the declared fact vocabulary (ROADMAP 5.2)]")
    d = tempfile.mkdtemp(prefix="aegis_reg_")
    reg = os.path.join(d, "predicates.json")
    with open(reg, "w") as fh:
        json.dump({
            "defaults_to": {"object": "string", "cardinality": "one"},
            "part_of": {"object": "id", "transitive": True,
                        "inverse_of": "contains"},
            "contains": {"object": "id", "inverse_of": "part_of"},
        }, fh)

    with Server(binary, port, phase=4,
                extra_args=["--predicate-registry", reg]) as srv:
        st = srv.req({"operation": "stats"})
        check(st["indexes"]["registered_predicates"] == 3,
              "stats reports the loaded vocabulary size")

        subj = srv.req({"operation": "insert", "type": "semantic",
                        "data": "subject"})["record"]["id"]

        def ins(fact):
            return srv.req({"operation": "insert", "type": "semantic",
                            "data": "d", "fact": fact})

        # declared predicate, declared object shape
        r = ins({"s": subj, "p": "defaults_to", "o": "none"})
        check(r.get("ok") is True, "a declared predicate is accepted")
        r = ins({"s": subj, "p": "part_of", "o": {"id": subj}})
        check(r.get("ok") is True, "an id-object predicate is accepted")

        # this is the whole point: an invented predicate is refused, so the
        # vocabulary cannot drift out from under whatever reads it later
        r = ins({"s": subj, "p": "invented_by_the_model", "o": "x"})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "an undeclared predicate is refused")

        # right predicate, wrong object shape — a literal where a record was
        # declared, and the reverse
        r = ins({"s": subj, "p": "defaults_to", "o": {"id": subj}})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "an id object for a string-declared predicate is refused")
        r = ins({"s": subj, "p": "part_of", "o": "not-an-id"})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "a literal for an id-declared predicate is refused")

        # a fact-less insert is unaffected by any of this
        r = srv.req({"operation": "insert", "type": "semantic", "data": "plain"})
        check(r.get("ok") is True, "a record with no fact is unaffected")

        # working memory is not a way around the vocabulary: it returns before
        # the persisted write path, so the check has to sit ahead of the type
        # dispatch rather than on it
        w = {"operation": "insert", "type": "working", "session_id": "s1",
             "data": "w"}
        r = srv.req(dict(w, fact={"s": subj, "p": "invented_here", "o": "x"}))
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "a working record cannot assert an undeclared predicate")
        r = srv.req(dict(w, fact={"s": subj, "p": "defaults_to", "o": "none"}))
        check(r.get("ok") is True,
              "and a declared one on working memory still works")

    # with no registry, any predicate is accepted: a server that never opted in
    # cannot be broken by this feature
    with Server(binary, port, phase=4) as srv:
        st = srv.req({"operation": "stats"})
        check(st["indexes"]["registered_predicates"] == 0,
              "no registry reports zero declared predicates")
        subj = srv.req({"operation": "insert", "type": "semantic",
                        "data": "s"})["record"]["id"]
        r = srv.req({"operation": "insert", "type": "semantic", "data": "d",
                     "fact": {"s": subj, "p": "anything_at_all", "o": "x"}})
        check(r.get("ok") is True, "without a registry any predicate is fine")


def test_predicate_registry_refuses_to_start(binary, port):
    print("[predicate registry: a bad vocabulary fails startup]")
    d = tempfile.mkdtemp(prefix="aegis_badreg_")

    def try_start(name, content):
        path = os.path.join(d, name)
        with open(path, "w") as fh:
            fh.write(content)
        datadir = tempfile.mkdtemp(prefix="aegis_badreg_data_")
        proc = subprocess.Popen(
            [binary, "--data-dir", datadir, "--port", str(port),
             "--predicate-registry", path],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            _, errout = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
            return None, b""
        return proc.returncode, errout

    # An operator who configured a registry is relying on it; degrading to
    # "accept everything" would be the opposite of what they asked for.
    rc, errout = try_start("dangling.json",
                           '{"p": {"object": "id", "inverse_of": "ghost"}}')
    check(rc is not None and rc != 0,
          "a dangling inverse_of refuses to start")
    check(b"ghost" in errout,
          "and the error names the offending predicate")

    rc, errout = try_start("typo.json",
                           '{"p": {"object": "id", "transative": true}}')
    check(rc is not None and rc != 0, "an unknown key refuses to start")
    check(b"transative" in errout, "naming the misspelt key")

    rc, _ = try_start("notjson.json", "{ this is not json")
    check(rc is not None and rc != 0, "malformed JSON refuses to start")

    # ...and a valid one starts, so the checks above are not just "it never
    # starts with a registry".
    rc, _ = try_start("good.json", '{"p": {"object": "id"}}')
    check(rc is None, "a valid registry starts and keeps running")


def test_pattern_search(binary, port):
    print("[search: the pattern filter over typed facts (ROADMAP 5.2)]")
    with Server(binary, port, phase=4) as srv:
        def ins(data, fact=None, **kw):
            p = {"operation": "insert", "type": "semantic", "data": data,
                 "include_embeddings": False}
            if fact is not None:
                p["fact"] = fact
            p.update(kw)
            return srv.req(p)["record"]["id"]

        def find(pattern, **kw):
            p = {"operation": "search", "pattern": pattern, "top_k": 50,
                 "include_embeddings": False}
            p.update(kw)
            return srv.req(p)

        def ids(r):
            return sorted(rec["id"] for rec in r.get("records", []))

        hook = ins("the recall hook")
        store = ins("the storage layer")
        hnsw = ins("hnsw.c")

        a = ins("hook defaults to none",
                {"s": hook, "p": "defaults_to", "o": "none"}, tags=["cfg"])
        b = ins("hnsw defaults to none",
                {"s": hnsw, "p": "defaults_to", "o": "none"})
        c = ins("hook is described as a hook",
                {"s": hook, "p": "described_as", "o": "a hook"})
        d = ins("hnsw is part of storage",
                {"s": hnsw, "p": "part_of", "o": {"id": store}})

        # each of the five bindable shapes
        check(ids(find({"s": hook})) == sorted([a, c]),
              "{s} finds everything about a subject")
        check(ids(find({"s": hook, "p": "defaults_to"})) == [a],
              "{s,p} narrows to one predicate")
        check(ids(find({"p": "defaults_to"})) == sorted([a, b]),
              "{p} finds every record using a predicate")
        check(ids(find({"o": "none"})) == sorted([a, b]),
              "{o} finds every record asserting a literal")
        check(ids(find({"p": "defaults_to", "o": "none"})) == sorted([a, b]),
              "{p,o} combines them")
        check(ids(find({"o": {"id": store}})) == [d],
              "an id-valued object is matched as a reference")

        # a wildcard is the same as omitting the position
        check(ids(find({"s": hook, "p": "*"})) == sorted([a, c]),
              'an explicit "*" is a wildcard')
        check(ids(find({"s": hook, "p": "*", "o": "*"})) == sorted([a, c]),
              "wildcards in every free position")

        # an id object and a literal that looks like one stay distinct
        check(ids(find({"o": str(store)})) == [],
              "a literal is not confused with an id reference")

        # misses
        check(ids(find({"s": 999999})) == [], "an unknown subject finds nothing")
        check(ids(find({"p": "never_used"})) == [],
              "an unknown predicate finds nothing")

        # records asserting nothing never match a pattern
        check(hook not in ids(find({"p": "*", "s": hook})),
              "a fact-less record does not match its own subject pattern")

        # intersects with the ordinary filters rather than replacing them
        check(ids(find({"p": "defaults_to"}, tags=["cfg"])) == [a],
              "a pattern intersects with a tag filter")
        check(ids(find({"p": "defaults_to"}, type="episodic")) == [],
              "and with a type filter")

        # a filter, not a ranking
        r = find({"p": "defaults_to"}, explain=True)
        ex = r["records"][0]["explain"]
        check(ex["semantic"] is False and ex["lexical"] is False,
              "a pattern hit is neither semantic nor lexical")

        # Combined with a ranked query the pattern acts on the ranked
        # candidates rather than replacing them: the vector search chooses what
        # to consider, the pattern removes what does not assert the right thing,
        # and the search widens its fetch to compensate for a selective filter.
        dim = 384
        vec = [1.0] + [0.0] * (dim - 1)
        srv.req({"operation": "insert", "type": "semantic", "data": "vectored",
                 "embedding": vec,
                 "fact": {"s": hook, "p": "defaults_to", "o": "none"}})
        r = find({"p": "defaults_to"}, embedding=vec)
        check(r.get("ok") is True and r.get("total", 0) >= 1,
              "a pattern composes with a semantic query")
        for rec in r.get("records", []):
            check(rec.get("fact", {}).get("p") == "defaults_to",
                  "every ranked hit still satisfies the pattern")
            break

        # count shares the candidate path, so it narrows too — and must agree
        # with what search returns for the same pattern, which is a stronger
        # claim than any literal expectation
        want = len(ids(find({"p": "defaults_to"})))
        r = srv.req({"operation": "count", "pattern": {"p": "defaults_to"}})
        check(r.get("ok") is True and r.get("count") == want,
              f"count honours a pattern and agrees with search ({want})")

        # --- validation ---
        r = find({})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "an empty pattern is refused")
        r = find({"s": "*", "p": "*", "o": "*"})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "an all-wildcard pattern is refused: it is a scan, not a filter")
        for bad in ("not-an-object", 42, [1]):
            r = find(bad)
            check(r.get("ok") is False
                  and r["error"]["code"] == "INVALID_REQUEST",
                  f"a non-object pattern {bad!r} is refused")
        r = find({"s": "forty-two"})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "a non-numeric subject is refused")
        # Out of range for a uint64: converting such a double is undefined, so
        # the parse has to reject it rather than let it through as a garbage
        # subject that silently matches the wrong facts. Verified to fail
        # against the unguarded cast.
        for bad in (-1, 1e30):
            r = find({"s": bad})
            check(r.get("ok") is False
                  and r["error"]["code"] == "INVALID_REQUEST",
                  f"an out-of-range subject {bad!r} is refused")
        check(srv.req({"operation": "ping"}).get("ok") is True,
              "and the server is still up afterwards")
        r = find({"o": {"nope": 1}})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "an object reference without an id is refused")

        # delete refuses a pattern rather than silently ignoring it: narrowing
        # or not, delete-by-pattern is a capability, not a side effect
        r = srv.req({"operation": "delete", "pattern": {"p": "defaults_to"}})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "bulk delete refuses a pattern instead of dropping it")
        check(len(ids(find({"p": "defaults_to"}))) == want,
              "and nothing was deleted")


def test_pattern_search_disabled_and_isolated(binary, port):
    print("[search: pattern needs the index, and respects tenants]")
    with Server(binary, port, phase=4,
                extra_args=["--no-fact-index"]) as srv:
        s0 = srv.req({"operation": "insert", "type": "semantic",
                      "data": "s"})["record"]["id"]
        srv.req({"operation": "insert", "type": "semantic", "data": "f",
                 "fact": {"s": s0, "p": "p", "o": "o"}})
        r = srv.req({"operation": "search", "pattern": {"p": "p"}})
        check(r.get("ok") is False and r["error"]["code"] == "NOT_READY",
              "a pattern without the index reports NOT_READY")
        r = srv.req({"operation": "search", "tags": ["x"]})
        check(r.get("ok") is True, "other searches are unaffected")

    tokens = ["acme-key   acme   rw", "beta-key   beta   rw", "admin-key admin"]
    with Server(binary, port, phase=4, token_lines=tokens) as srv:
        def ins(tok, data, fact=None):
            p = {"operation": "insert", "type": "semantic", "data": data,
                 "token": tok}
            if fact:
                p["fact"] = fact
            return srv.req(p)["record"]["id"]

        subj = ins("admin-key", "shared subject")
        mine = ins("acme-key", "acme asserts", {"s": subj, "p": "p", "o": "o"})
        theirs = ins("beta-key", "beta asserts", {"s": subj, "p": "p", "o": "o"})

        # Both tenants assert the same triple about the same subject. A pattern
        # search must return only the asserting records the caller owns — the
        # fact index is global, so this is the isolation that matters.
        r = srv.req({"operation": "search", "pattern": {"s": subj, "p": "p"},
                     "token": "acme-key", "include_embeddings": False})
        got = sorted(rec["id"] for rec in r.get("records", []))
        check(got == [mine], "a tenant sees only its own assertion")
        check(theirs not in got, "and never a co-tenant's")

        r = srv.req({"operation": "search", "pattern": {"s": subj, "p": "p"},
                     "token": "admin-key", "include_embeddings": False})
        check(sorted(rec["id"] for rec in r.get("records", []))
              == sorted([mine, theirs]), "an admin sees both")


def test_consolidate(binary, port):
    print("[consolidate: merge near-duplicate semantic memories]")
    with Server(binary, port, phase=4) as srv:
        # three near-identical vectors + one distinct
        def ins(vec, tag):
            r = srv.req({"operation": "insert", "type": "semantic",
                         "tags": [tag], "data": "d", "embedding": vec})
            return r["record"]["id"]
        dim = 384
        base = [1.0] + [0.0] * (dim - 1)
        near1 = [0.999, 0.001] + [0.0] * (dim - 2)
        near2 = [0.998, 0.002] + [0.0] * (dim - 2)
        far = [0.0, 1.0] + [0.0] * (dim - 2)
        aid = ins(base, "a"); bid = ins(near1, "b"); cid = ins(near2, "c")
        cluster = {aid, bid, cid}
        distinct = ins(far, "z")

        r = srv.req({"operation": "consolidate", "min_similarity": 0.95})
        check(r.get("ok") is True and r.get("clusters") == 1 and r.get("merged") == 2,
              "consolidate merges the 3-vector cluster (2 merged away)")

        # the distinct record survives
        r = srv.req({"operation": "get", "id": distinct})
        check(r.get("ok") is True, "distinct record left untouched")

        # a count of the near-dup tags shows only the survivor remains
        n = srv.req({"operation": "count", "tags": ["a", "b", "c"], "match": "any"})
        check(n.get("ok") is True and n.get("count") == 1,
              "one survivor carries the merged cluster")

        # provenance (ROADMAP 2.2): the survivor records `supersedes` links to the
        # two records it absorbed, so the merge is auditable rather than silent.
        s = srv.req({"operation": "search", "tags": ["a", "b", "c"],
                     "match": "any", "top_k": 5})
        recs = s.get("records", [])
        sv = recs[0] if recs else {}
        sup = sorted(x["to_id"] for x in sv.get("relationships", [])
                     if x.get("kind") == "supersedes")
        check(sup == sorted(cluster - {sv.get("id")}),
              "survivor supersedes the 2 merged ids (auditable lineage)")

        # idempotent
        r = srv.req({"operation": "consolidate", "min_similarity": 0.95})
        check(r.get("ok") is True and r.get("merged") == 0,
              "second consolidate is a no-op")


def test_forget(binary, port):
    print("[forget: decay-based forgetting (ROADMAP 2.3)]")
    with Server(binary, port, phase=4) as srv:
        def ins(t, imp, tag, typ="episodic"):
            r = srv.req({"operation": "insert", "type": typ, "data": "x",
                         "importance": imp, "tags": [tag]})
            return r["record"]["id"]
        keep = ins("k", 0.9, "keep")           # high importance -> retained
        low = [ins("n", 0.01, "noise") for _ in range(4)]  # low -> forgotten
        sem = ins("s", 0.01, "fact", "semantic")           # protected by type

        # dry_run: reports what WOULD go, deletes nothing
        r = srv.req({"operation": "forget", "min_retention": 0.05, "dry_run": True})
        check(r.get("ok") is True and r.get("dry_run") is True and r.get("forgotten") == 4,
              "dry_run reports 4 would-forget without deleting")
        n = srv.req({"operation": "count", "tags": ["noise"], "match": "any"})
        check(n.get("count") == 4, "dry_run left the low-value records in place")

        # real forget: low-importance episodic tombstoned
        r = srv.req({"operation": "forget", "min_retention": 0.05})
        check(r.get("ok") is True and r.get("forgotten") == 4 and r.get("dry_run") is False,
              "forget tombstones the 4 low-value episodic records")
        check(srv.req({"operation": "get", "id": keep}).get("ok") is True,
              "high-importance record is retained")
        check(srv.req({"operation": "get", "id": low[0]}).get("ok") is False,
              "low-importance record is forgotten")
        check(srv.req({"operation": "get", "id": sem}).get("ok") is True,
              "semantic record is protected (default type=episodic)")

        # idempotent: nothing left below the threshold
        r = srv.req({"operation": "forget", "min_retention": 0.05})
        check(r.get("forgotten") == 0, "second forget is a no-op")

        # max_forget caps deletions
        for _ in range(5):
            ins("n2", 0.01, "noise2")
        r = srv.req({"operation": "forget", "min_retention": 0.05, "max_forget": 2})
        check(r.get("forgotten") == 2, "max_forget caps the number tombstoned")


def test_memory_quality_metrics(binary, port):
    print("[metrics: memory-quality outcomes (ROADMAP 3.3)]")
    with Server(binary, port, phase=4) as srv:
        m0 = srv.req({"operation": "stats"})["metrics"]
        check(m0.get("memories_forgotten") == 0 and m0.get("memories_merged") == 0
              and m0.get("memories_purged") == 0, "outcome counters start at zero")

        # forget: low-value episodic age out
        for i in range(3):
            srv.req({"operation": "insert", "type": "episodic",
                     "data": f"noise {i}", "importance": 0.01})
        srv.req({"operation": "forget", "min_retention": 0.05})

        # consolidate: near-duplicate semantic merge
        dim = 384
        base = [1.0] + [0.0] * (dim - 1)
        near = [0.999, 0.001] + [0.0] * (dim - 2)
        srv.req({"operation": "insert", "type": "semantic", "data": "d", "embedding": base})
        srv.req({"operation": "insert", "type": "semantic", "data": "d", "embedding": near})
        srv.req({"operation": "consolidate", "min_similarity": 0.95})

        # purge: erase a namespace
        for i in range(2):
            srv.req({"operation": "insert", "type": "episodic", "data": f"a{i}",
                     "agent_id": "alice"})
        srv.req({"operation": "purge", "agent_id": "alice"})

        m = srv.req({"operation": "stats"})["metrics"]
        check(m.get("memories_forgotten") == 3, "memories_forgotten counts forget")
        check(m.get("memories_merged") == 1, "memories_merged counts consolidate")
        check(m.get("memories_purged") == 2, "memories_purged counts purge")

        # a dry-run must not move the counters
        srv.req({"operation": "insert", "type": "episodic", "data": "z", "importance": 0.01})
        srv.req({"operation": "forget", "min_retention": 0.05, "dry_run": True})
        m2 = srv.req({"operation": "stats"})["metrics"]
        check(m2.get("memories_forgotten") == 3, "dry-run forget does not increment the counter")


def test_temporal(binary, port):
    print("[temporal: history + point-in-time get (ROADMAP 3.1)]")
    with Server(binary, port, phase=4) as srv:
        r = srv.req({"operation": "insert", "type": "semantic", "data": "sky is blue"})
        rid, t1 = r["record"]["id"], r["record"]["updated"]
        time.sleep(0.01)
        t2 = srv.req({"operation": "update", "id": rid, "data": "sky is azure"})["record"]["updated"]
        time.sleep(0.01)
        t3 = srv.req({"operation": "update", "id": rid, "data": "sky is grey"})["record"]["updated"]
        check(t1 < t2 < t3, "each version has a distinct, increasing timestamp")

        # history: the full version trail with validity intervals
        h = srv.req({"operation": "history", "id": rid})
        check(h.get("ok") and h.get("count") == 3, "history returns every version")
        vs = h["versions"]
        check([v["data"] for v in vs] == ["sky is blue", "sky is azure", "sky is grey"],
              "versions are in causal order")
        check(vs[0]["valid_to"] == vs[1]["valid_from"] == t2,
              "validity intervals chain (valid_to == next valid_from)")
        check(vs[-1]["valid_to"] == 0, "the current version has an open validity interval")

        # point-in-time get: "what did the agent know at time T?"
        check(srv.req({"operation": "get", "id": rid, "as_of": t1})["record"]["data"] == "sky is blue",
              "as_of t1 -> the original")
        check(srv.req({"operation": "get", "id": rid, "as_of": t2})["record"]["data"] == "sky is azure",
              "as_of t2 -> the second version")
        check(srv.req({"operation": "get", "id": rid, "as_of": t1 - 1})["error"]["code"] == "NOT_FOUND",
              "as_of before creation -> NOT_FOUND")

        # delete, then the past is still reconstructable while the present is gone
        time.sleep(0.01)
        srv.req({"operation": "delete", "id": rid})
        check(srv.req({"operation": "get", "id": rid})["error"]["code"] == "NOT_FOUND",
              "current get after delete -> NOT_FOUND")
        check(srv.req({"operation": "get", "id": rid, "as_of": t3})["record"]["data"] == "sky is grey",
              "as_of before the delete still reconstructs the record")
        hd = srv.req({"operation": "history", "id": rid})
        check(hd["count"] == 4 and hd["versions"][-1]["deleted"] is True,
              "history includes the tombstone as the final version")

        check(srv.req({"operation": "history", "id": 999999})["error"]["code"] == "NOT_FOUND",
              "history of an unknown id -> NOT_FOUND")


def test_temporal_isolation(binary, port):
    print("[temporal: tenant isolation]")
    lines = ["admintok", "acme_rw acme rw", "beta_rw beta rw"]
    with Server(binary, port, phase=4, token_lines=lines) as srv:
        rid = srv.req({"operation": "insert", "type": "semantic", "data": "acme fact",
                       "token": "acme_rw"})["record"]["id"]
        # another tenant can neither read its history nor reconstruct it
        check(srv.req({"operation": "history", "id": rid, "token": "beta_rw"})["error"]["code"] == "NOT_FOUND",
              "cross-tenant history -> NOT_FOUND")
        check(srv.req({"operation": "get", "id": rid, "as_of": 9999999999999,
                       "token": "beta_rw"})["error"]["code"] == "NOT_FOUND",
              "cross-tenant as_of get -> NOT_FOUND")
        # the owner can
        check(srv.req({"operation": "history", "id": rid, "token": "acme_rw"})["count"] == 1,
              "owner reads its own history")


def test_export_and_purge(binary, port):
    print("[export + right-to-be-forgotten (ROADMAP 3.2)]")
    with Server(binary, port, phase=4) as srv:
        for i in range(3):
            srv.req({"operation": "insert", "type": "episodic",
                     "data": f"ALICE_SECRET_{i} likes coffee", "agent_id": "alice"})
        for i in range(2):
            srv.req({"operation": "insert", "type": "episodic",
                     "data": f"BOB_DATA_{i}", "agent_id": "bob"})

        # export a subject's records
        r = srv.req({"operation": "export", "agent_id": "alice", "limit": 10})
        check(r.get("ok") and r.get("count") == 3 and r.get("namespace") == "alice",
              "export returns the subject's records")
        check(all("ALICE_SECRET" in rec["data"] for rec in r["records"]),
              "export returns only the subject's data")

        # pagination via cursor
        p1 = srv.req({"operation": "export", "agent_id": "alice", "limit": 2})
        check(p1.get("count") == 2 and p1.get("has_more") is True, "export page 1")
        p2 = srv.req({"operation": "export", "agent_id": "alice", "limit": 2,
                      "after_id": p1["cursor"]})
        check(p2.get("count") == 1 and p2.get("has_more") is False, "export page 2")

        # a subjectless export/purge is refused (no "dump/erase everything")
        check(srv.req({"operation": "export"})["error"]["code"] == "INVALID_REQUEST",
              "subjectless export rejected")
        check(srv.req({"operation": "purge"})["error"]["code"] == "INVALID_REQUEST",
              "subjectless purge rejected")

        # dry-run purge counts without deleting
        d = srv.req({"operation": "purge", "agent_id": "alice", "dry_run": True})
        check(d.get("purged") == 3 and d.get("dry_run") is True and d.get("compacted") is False,
              "dry-run purge counts without deleting")
        check(srv.req({"operation": "export", "agent_id": "alice"})["count"] == 3,
              "dry-run left the data in place")

        # real purge + compaction
        r = srv.req({"operation": "purge", "agent_id": "alice"})
        check(r.get("purged") == 3 and r.get("compacted") is True,
              "purge tombstones the subject and compacts")
        check(srv.req({"operation": "export", "agent_id": "alice"})["count"] == 0,
              "subject's data is gone after purge")
        check(srv.req({"operation": "export", "agent_id": "bob"})["count"] == 2,
              "other tenants are untouched")

        # COMPLIANCE PROOF: the purged plaintext is gone from the on-disk log
        # (compaction rewrote it); a co-tenant's data survives.
        with open(os.path.join(srv.datadir, "memory.log"), "rb") as fh:
            blob = fh.read()
        check(b"ALICE_SECRET" not in blob,
              "purged plaintext is absent from the on-disk log after compaction")
        check(b"BOB_DATA" in blob, "co-tenant data survives in the log")


def test_export_purge_isolation(binary, port):
    print("[export/purge: tenant isolation]")
    lines = ["admintok", "acme_rw acme rw", "acme_ro acme ro", "beta_rw beta rw"]
    with Server(binary, port, phase=4, token_lines=lines) as srv:
        srv.req({"operation": "insert", "type": "episodic", "data": "acme one",
                 "token": "acme_rw"})
        srv.req({"operation": "insert", "type": "episodic", "data": "beta one",
                 "token": "beta_rw"})

        # a namespaced token exports only its own ns, even if it spoofs agent_id
        r = srv.req({"operation": "export", "agent_id": "beta", "token": "acme_rw"})
        check(r.get("ok") and r.get("count") == 1 and r["records"][0]["data"] == "acme one",
              "export is pinned to the token's namespace (spoofed agent_id ignored)")

        # a read-only token cannot purge
        check(srv.req({"operation": "purge", "token": "acme_ro"})["error"]["code"] == "FORBIDDEN",
              "read-only token cannot purge")

        # acme purges its own ns; beta is untouched
        r = srv.req({"operation": "purge", "token": "acme_rw"})
        check(r.get("ok") and r.get("purged") == 1, "tenant purges its own namespace")
        check(srv.req({"operation": "export", "token": "acme_rw"})["count"] == 0,
              "acme data gone")
        check(srv.req({"operation": "export", "token": "beta_rw"})["count"] == 1,
              "beta data untouched by acme's purge")


def test_snapshot(binary, port):
    print("[backup: online snapshot + recover]")
    with Server(binary, port, phase=4) as srv:
        for i in range(3):
            srv.req({"operation": "insert", "type": "episodic",
                     "data": f"note {i}", "tags": ["backup"]})
        # named snapshot reports where it landed and what it covers
        r = srv.req({"operation": "snapshot", "name": "b1"})
        check(r.get("ok") is True, "snapshot ok")
        check(r.get("record_count") == 3, "snapshot counts live records")
        check(isinstance(r.get("log_size"), int) and r["log_size"] > 0,
              "snapshot reports covered log size")
        check(r.get("next_id") == 4, "snapshot reports next_id high-water")
        snap_dir = r["snapshot"]

        # the snapshot dir is a self-contained, restorable data set
        for f in ("memory.log", "metadata.db", "manifest.json"):
            check(os.path.exists(os.path.join(snap_dir, f)),
                  f"snapshot contains {f}")
        with open(os.path.join(snap_dir, "manifest.json")) as fh:
            man = json.load(fh)
        check(man.get("log_size") == r["log_size"] and man.get("next_id") == 4,
              "manifest matches the response")

        # default (unnamed) snapshot works too
        r = srv.req({"operation": "snapshot"})
        check(r.get("ok") is True and r["snapshot"] != snap_dir,
              "unnamed snapshot uses a generated name")

        # a name with a path separator is rejected (no traversal)
        r = srv.req({"operation": "snapshot", "name": "../evil"})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "traversal name rejected")

    # recover: a fresh server pointed at the snapshot dir rebuilds from it
    restore = Server(binary, port + 1, phase=4)
    restore.datadir = snap_dir
    with restore:
        r = restore.req({"operation": "search", "top_k": 100})
        check(r.get("ok") is True and r.get("total") == 3,
              "recovered all records from the snapshot")
        # next_id floor survives, so a new insert does not reuse an id
        r = restore.req({"operation": "insert", "type": "episodic", "data": "new"})
        check(r.get("ok") is True and r["record"]["id"] == 4,
              "recovered next_id floor prevents id reuse")


def test_snapshot_admin_only(binary, port):
    print("[backup: snapshot is admin-only]")
    lines = ["admintok", "acme_rw acme rw", "acme_ro acme ro"]
    with Server(binary, port, phase=4, token_lines=lines) as srv:
        r = srv.req({"operation": "snapshot", "name": "x", "token": "acme_rw"})
        check(r.get("ok") is False and r["error"]["code"] == "FORBIDDEN",
              "namespaced token cannot snapshot")
        r = srv.req({"operation": "snapshot", "name": "x", "token": "acme_ro"})
        check(r.get("ok") is False and r["error"]["code"] == "FORBIDDEN",
              "read-only token cannot snapshot")
        r = srv.req({"operation": "snapshot", "name": "x"})  # no token
        check(r.get("ok") is False and r["error"]["code"] == "UNAUTHORIZED",
              "unauthenticated snapshot rejected")
        r = srv.req({"operation": "snapshot", "name": "x", "token": "admintok"})
        check(r.get("ok") is True, "admin token can snapshot")


def test_restore(binary, port):
    print("[backup: --restore installs a snapshot]")

    def run_restore(snap, dest, extra=None):
        args = [binary, "--restore", snap, "--data-dir", dest] + (extra or [])
        return subprocess.run(args, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode

    # produce a snapshot from a live server
    with Server(binary, port, phase=4) as srv:
        for i in range(4):
            srv.req({"operation": "insert", "type": "episodic", "data": f"r{i}"})
        snap = srv.req({"operation": "snapshot", "name": "snap"})["snapshot"]

    # restore into a fresh (absent) data dir via the one-shot CLI mode
    dest = tempfile.mkdtemp(prefix="aegis_restore_")
    shutil.rmtree(dest)
    check(run_restore(snap, dest) == 0, "restore into a fresh dir exits 0")
    check(os.path.exists(os.path.join(dest, "memory.log")),
          "restore installs memory.log")

    # a server started on the restored dir sees every record
    s2 = Server(binary, port + 1, phase=4)
    s2.datadir = dest
    with s2:
        r = s2.req({"operation": "search", "top_k": 100})
        check(r.get("ok") is True and r.get("total") == 4,
              "restored server has all records")
        r = s2.req({"operation": "insert", "type": "episodic", "data": "new"})
        check(r["record"]["id"] == 5, "restored next_id floor prevents id reuse")

    # refuses to clobber an existing database
    check(run_restore(snap, dest) == 1, "restore refuses to overwrite a db")

    # rejects an embedding-dim that does not match the snapshot
    dest2 = tempfile.mkdtemp(prefix="aegis_restore_")
    shutil.rmtree(dest2)
    check(run_restore(snap, dest2, ["--embedding-dim", "128"]) == 1,
          "restore rejects an embedding-dim mismatch")
    check(not os.path.exists(os.path.join(dest2, "memory.log")),
          "rejected restore leaves the target empty")

    # a directory that is not a snapshot is rejected
    notsnap = tempfile.mkdtemp(prefix="aegis_notsnap_")
    dest3 = tempfile.mkdtemp(prefix="aegis_restore_")
    shutil.rmtree(dest3)
    check(run_restore(notsnap, dest3) == 1, "restore rejects a non-snapshot dir")


def test_encrypted_backup_restore(binary, port):
    print("[backup: encrypted snapshot restores only with the matching key]")
    datadir = tempfile.mkdtemp(prefix="aegis_encbk_")
    keyfile = os.path.join(datadir, "key.hex")
    with open(keyfile, "w") as fh:
        fh.write("0f1e2d3c4b5a69788796a5b4c3d2e1f0"
                 "0f1e2d3c4b5a69788796a5b4c3d2e1f0\n")
    badkey = os.path.join(datadir, "bad.hex")
    with open(badkey, "w") as fh:
        fh.write("11" * 32 + "\n")

    def run_restore(snap, dest, extra=None):
        args = [binary, "--restore", snap, "--data-dir", dest] + (extra or [])
        return subprocess.run(args, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode

    enc = ["--encryption-key-file", keyfile]
    with Server(binary, port, datadir=datadir, extra_args=enc) as srv:
        for i in range(3):
            srv.req({"operation": "insert", "type": "episodic", "data": f"e{i}"})
        snap = srv.req({"operation": "snapshot", "name": "snap"})["snapshot"]

    # the manifest marks the snapshot encrypted and names the key fingerprint
    with open(os.path.join(snap, "manifest.json")) as fh:
        man = json.load(fh)
    check(man.get("encrypted") is True and isinstance(man.get("key_fingerprint"),
          str), "manifest records encrypted + key fingerprint")

    # restore with the matching key succeeds; the records recover under the key
    dest = tempfile.mkdtemp(prefix="aegis_encbk_r_")
    shutil.rmtree(dest)
    check(run_restore(snap, dest, enc) == 0, "restore with the matching key -> 0")
    s2 = Server(binary, port + 1, datadir=dest, extra_args=enc)
    with s2:
        r = s2.req({"operation": "search", "top_k": 100})
        check(r.get("ok") is True and r.get("total") == 3,
              "restored encrypted db has all records")

    # restore without a key, or with the wrong key, is refused (nothing installed)
    d_nokey = tempfile.mkdtemp(prefix="aegis_encbk_n_"); shutil.rmtree(d_nokey)
    check(run_restore(snap, d_nokey) == 1, "restore of an encrypted snapshot needs a key")
    check(not os.path.exists(os.path.join(d_nokey, "memory.log")),
          "refused (no key) leaves the target empty")
    d_bad = tempfile.mkdtemp(prefix="aegis_encbk_b_"); shutil.rmtree(d_bad)
    check(run_restore(snap, d_bad, ["--encryption-key-file", badkey]) == 1,
          "restore with the wrong key is refused")


def test_multivector(binary, port):
    print("[multi-vector: embeddings array round-trip]")
    with Server(binary, port, phase=4) as srv:  # --embedding-dim 384
        dim = 384
        v0 = [1.0] + [0.0] * (dim - 1)
        v1 = [0.0, 1.0] + [0.0] * (dim - 2)
        r = srv.req({"operation": "insert", "type": "semantic", "tags": ["mv"],
                     "data": "doc", "embeddings": [v0, v1]})
        check(r.get("ok") is True, "insert with embeddings ok")
        rid = r["record"]["id"]
        # the record echoes both vectors
        r = srv.req({"operation": "get", "id": rid})
        embs = r.get("record", {}).get("embeddings")
        check(isinstance(embs, list) and len(embs) == 2 and len(embs[0]) == dim,
              "get echoes both embeddings")
        # best-of-N: found by EITHER of its vectors, returned once
        for label, q in (("primary", v0), ("secondary", v1)):
            r = srv.req({"operation": "search", "embedding": q, "top_k": 5})
            hits = [rec["id"] for rec in r.get("records", []) if rec["id"] == rid]
            check(len(hits) == 1, f"found once by its {label} vector")
        # a vector of the wrong dimension is rejected
        r = srv.req({"operation": "insert", "type": "semantic", "data": "bad",
                     "embeddings": [[1.0, 2.0, 3.0]]})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "wrong-dimension embedding rejected")


def test_include_embeddings(binary, port):
    print("[include_embeddings: response shaping]")
    with Server(binary, port, phase=4) as srv:  # default --embedding-dim 384
        dim = 384
        single = [1.0] + [0.0] * (dim - 1)
        multi = [[1.0] + [0.0] * (dim - 1), [0.0, 1.0] + [0.0] * (dim - 2)]

        r = srv.req({"operation": "insert", "type": "semantic", "tags": ["ie"],
                     "data": "one", "embedding": single})
        sid = r["record"]["id"]
        r = srv.req({"operation": "insert", "type": "semantic", "tags": ["ie"],
                     "data": "many", "embeddings": multi})
        mid = r["record"]["id"]

        # default: embeddings are present (backward compatible)
        r = srv.req({"operation": "get", "id": sid})
        check("embedding" in r.get("record", {}),
              "get includes embedding by default")

        # include_embeddings=false omits them but keeps the rest
        r = srv.req({"operation": "get", "id": sid, "include_embeddings": False})
        rec = r.get("record", {})
        check(rec.get("ok", True) is not False and "embedding" not in rec,
              "get omits single embedding when include_embeddings=false")
        check(rec.get("data") == "one" and rec.get("tags") == ["ie"],
              "omitting embeddings preserves data/tags")

        # multi-vector "embeddings" array is omitted too
        r = srv.req({"operation": "get", "id": mid, "include_embeddings": False})
        check("embeddings" not in r.get("record", {}),
              "get omits multi-vector embeddings when include_embeddings=false")

        # search honors the flag while still ranking by the query vector
        r = srv.req({"operation": "search", "embedding": single, "top_k": 5,
                     "include_embeddings": False})
        recs = r.get("records", [])
        check(any(x["id"] == sid for x in recs), "search still returns the match")
        check(all("embedding" not in x and "embeddings" not in x for x in recs),
              "search omits embeddings from every record when false")

        # explicit true behaves like the default
        r = srv.req({"operation": "get", "id": sid, "include_embeddings": True})
        check("embedding" in r.get("record", {}),
              "include_embeddings=true includes the embedding")


def test_search_explain(binary, port):
    print("[search: per-hit ranking explanation (ROADMAP 1.2)]")
    with Server(binary, port, phase=4) as srv:  # default --embedding-dim 384
        dim = 384
        va = [1.0] + [0.0] * (dim - 1)
        vb = [0.0, 1.0] + [0.0] * (dim - 2)
        srv.req({"operation": "insert", "type": "semantic", "data": "alpha",
                 "importance": 0.9, "confidence": 1.0, "embedding": va})
        srv.req({"operation": "insert", "type": "semantic", "data": "beta",
                 "importance": 0.3, "confidence": 1.0, "embedding": vb})

        # default: no explain block (responses stay lean)
        r = srv.req({"operation": "search", "embedding": va, "top_k": 2})
        check(all("explain" not in rec for rec in r.get("records", [])),
              "explain omitted by default")

        # explain=true: every hit carries a breakdown
        r = srv.req({"operation": "search", "embedding": va, "top_k": 2,
                     "explain": True, "include_embeddings": False})
        recs = r.get("records", [])
        check(len(recs) == 2 and all("explain" in rec for rec in recs),
              "explain present on every hit when requested")
        top = recs[0]
        e = top.get("explain", {})
        check(top.get("data") == "alpha", "higher importance+similarity ranks first")
        check(e.get("semantic") is True, "explain marks semantic ranking")
        for f in ("score", "similarity", "importance", "confidence", "weight",
                  "recency_factor"):
            check(f in e, f"explain has {f}")
        # score == weight * similarity * recency_factor
        expect = e["weight"] * e["similarity"] * e["recency_factor"]
        check(abs(e["score"] - expect) < 1e-4,
              "explain score == weight * similarity * recency_factor")
        check(abs(e["importance"] - 0.9) < 1e-4, "explain reports the record importance")

        # recency decay: a half-life shrinks recency_factor below 1. The factor
        # is 0.5**(age_ms/half_life_ms), so it is exactly 1.0 when age is 0 —
        # sleep so the record is measurably older than "now" (otherwise a fast
        # insert->search round-trip within the same millisecond makes age 0 and
        # the factor 1.0, a spurious failure seen on faster builds).
        time.sleep(0.01)
        r = srv.req({"operation": "search", "embedding": va, "top_k": 1,
                     "explain": True, "half_life_ms": 1})
        e = r["records"][0]["explain"]
        check(e["recency_factor"] < 1.0,
              "recency_factor < 1 under an aggressive half_life_ms")


def test_lexical_search(binary, port):
    print("[search: lexical (BM25) keyword query (ROADMAP 4.1)]")
    with Server(binary, port, phase=4) as srv:
        docs = [
            "per-tenant quotas are set with --tenant-max-records at startup",
            "the CRC framing bug was fixed in hnsw.c:214 last release",
            "the build needs a clean rebuild after editing any header",
            "we rejected the blue-green migration approach as too costly",
        ]
        ids = []
        for d in docs:
            r = srv.req({"operation": "insert", "type": "semantic", "data": d,
                         "tags": ["note"], "importance": 0.8})
            ids.append(r["record"]["id"])

        # The point of the feature: an exact identifier is retrievable. These are
        # precisely the terms a dense embedding averages away.
        for q, want in (("--tenant-max-records", ids[0]),
                        ("hnsw.c:214", ids[1]),
                        ("tenant-max-records", ids[0])):
            r = srv.req({"operation": "search", "query": q, "top_k": 3})
            got = [rec["id"] for rec in r.get("records", [])]
            check(got[:1] == [want], f"query {q!r} ranks its record first")

        # A compound identifier is also findable by one of its words.
        r = srv.req({"operation": "search", "query": "quotas", "top_k": 3})
        check(ids[0] in [rec["id"] for rec in r["records"]],
              "compound term findable by a sub-word")

        # A miss is an empty result, not an error.
        r = srv.req({"operation": "search", "query": "zzz_nonexistent", "top_k": 3})
        check(r.get("ok") is True and r.get("records") == [],
              "unmatched query returns an empty result set")

        # Ordinary filters still compose with a text query.
        r = srv.req({"operation": "search", "query": "quotas",
                     "tags": ["absent-tag"], "top_k": 3})
        check(r.get("records") == [], "tag filter still applies to a text query")
        r = srv.req({"operation": "search", "query": "quotas",
                     "type": "episodic", "top_k": 3})
        check(r.get("records") == [], "type filter still applies to a text query")

        # explain reports the BM25 contribution and the score identity.
        r = srv.req({"operation": "search", "query": "clean rebuild header",
                     "top_k": 1, "explain": True})
        e = r["records"][0]["explain"]
        check(e.get("lexical") is True, "explain marks a lexical hit")
        check(e.get("semantic") is False, "lexical-only hit is not marked semantic")
        check(e.get("bm25", 0) > 0, "explain reports a positive bm25 score")
        check("similarity" not in e, "no cosine reported for a lexical-only hit")
        expect = e["weight"] * e["bm25"] * e["recency_factor"]
        check(abs(e["score"] - expect) < 1e-4,
              "explain score == weight * bm25 * recency_factor")

        # An update re-indexes: the superseded text stops matching.
        srv.req({"operation": "update", "id": ids[3],
                 "data": "we adopted the canary rollout plan instead"})
        r = srv.req({"operation": "search", "query": "blue-green", "top_k": 3})
        check(r.get("records") == [], "updated-away text no longer matches")
        r = srv.req({"operation": "search", "query": "canary rollout", "top_k": 3})
        check([rec["id"] for rec in r["records"]] == [ids[3]],
              "replacement text matches after update")

        # A delete unindexes.
        srv.req({"operation": "delete", "id": ids[2]})
        r = srv.req({"operation": "search", "query": "clean rebuild", "top_k": 3})
        check(r.get("records") == [], "deleted record no longer matches")

        # stats expose the index size so its RAM can be watched like the others.
        s = srv.req({"operation": "stats"})
        check(s["indexes"]["lexical_docs"] == 3,
              "stats reports live indexed documents")
        check(s["indexes"]["lexical_terms"] > 0, "stats reports distinct terms")
        check(s["memory"]["lexical_bytes"] > 0, "stats reports lexical index bytes")


def test_lexical_hybrid(binary, port):
    print("[search: hybrid lexical + semantic fusion (ROADMAP 4.1)]")
    with Server(binary, port, phase=4) as srv:  # default --embedding-dim 384
        dim = 384
        va = [1.0] + [0.0] * (dim - 1)
        vb = [0.0, 1.0] + [0.0] * (dim - 2)
        # Three records covering the three ways a hybrid query can match:
        #   near  - the closest vector, but no query term    (semantic only)
        #   both  - a weaker vector AND the query term       (both sources)
        #   exact - the query term, no embedding at all      (lexical only)
        # `exact` is the case an embeddings-only server cannot retrieve at all.
        near = srv.req({"operation": "insert", "type": "semantic",
                        "data": "notes about vectors and similarity",
                        "importance": 0.9, "embedding": va})["record"]["id"]
        both = srv.req({"operation": "insert", "type": "semantic",
                        "data": "raise --tenant-rate-qps for the busy tenant",
                        "importance": 0.9, "embedding": vb})["record"]["id"]
        exact = srv.req({"operation": "insert", "type": "semantic",
                         "data": "the flag is --tenant-rate-qps in the docs",
                         "importance": 0.9})["record"]["id"]

        # Semantic alone cannot retrieve the embedding-less record at all.
        r = srv.req({"operation": "search", "embedding": va, "top_k": 5,
                     "include_embeddings": False})
        got = [rec["id"] for rec in r["records"]]
        check(exact not in got,
              "semantic-only cannot retrieve the embedding-less record")
        check(got[0] == near, "semantic-only ranks the nearest vector first")

        # The core RRF property: agreeing with both sources outranks matching one,
        # even though `both` is only the *second*-nearest vector.
        r = srv.req({"operation": "search", "query": "--tenant-rate-qps",
                     "embedding": va, "top_k": 5, "explain": True,
                     "include_embeddings": False})
        recs = r["records"]
        check(recs[0]["id"] == both,
              "hybrid ranks the record both sources found first")
        check(exact in [rec["id"] for rec in recs],
              "hybrid retrieves the lexical-only record the vectors cannot see")

        top = recs[0]["explain"]
        check(top.get("semantic") is True and top.get("lexical") is True,
              "the fused hit is marked as found by both sources")
        check(top.get("lexical_rank") == 1 and top.get("semantic_rank") == 2,
              "the fused hit reports its rank in each source list")
        check(top.get("rrf", 0) > 0, "hybrid hit carries a fused rrf score")
        expect = top["weight"] * top["rrf"] * top["recency_factor"]
        check(abs(top["score"] - expect) < 1e-4,
              "explain score == weight * rrf * recency_factor")

        by_id = {rec["id"]: rec["explain"] for rec in recs}
        lex_only = by_id[exact]
        check(lex_only.get("lexical") is True and lex_only.get("semantic") is False,
              "the embedding-less record is marked lexical-only")
        check(lex_only.get("semantic_rank") == 0,
              "a lexical-only hit reports semantic_rank 0, not an absent field")
        sem_only = by_id[near]
        check(sem_only.get("semantic") is True and sem_only.get("lexical") is False,
              "the term-less record is marked semantic-only")
        check(sem_only.get("lexical_rank") == 0,
              "a semantic-only hit reports lexical_rank 0")

        # min_score still gates on the cosine in a hybrid query. Query with a
        # vector at 45 degrees to `near` (cosine ~0.707) and set the floor above
        # it: the semantic side is emptied while the lexical hit is untouched.
        vq = [1.0, 1.0] + [0.0] * (dim - 2)
        r = srv.req({"operation": "search", "query": "--tenant-rate-qps",
                     "embedding": vq, "top_k": 5, "min_score": 0.9,
                     "include_embeddings": False})
        got = [rec["id"] for rec in r["records"]]
        check(near not in got, "min_score drops the semantic side of a hybrid query")
        check(exact in got, "the lexical side survives min_score")
        # Without the floor the same query returns both.
        r = srv.req({"operation": "search", "query": "--tenant-rate-qps",
                     "embedding": vq, "top_k": 5, "include_embeddings": False})
        check(near in [rec["id"] for rec in r["records"]],
              "the semantic hit returns once the floor is removed")


def test_lexical_disabled(binary, port):
    print("[search: --no-lexical-index opt-out]")
    with Server(binary, port, phase=4,
                extra_args=["--no-lexical-index"]) as srv:
        srv.req({"operation": "insert", "type": "semantic",
                 "data": "quotas via --tenant-max-records", "tags": ["note"]})
        # A text query must fail loudly rather than silently degrade to an
        # unranked filter scan that looks like a legitimate empty result.
        r = srv.req({"operation": "search", "query": "quotas", "top_k": 3})
        check(r.get("ok") is False, "query rejected when the index is disabled")
        check(r.get("error", {}).get("code") == "NOT_READY",
              "disabled lexical index reports NOT_READY")
        check("lexical" in r.get("error", {}).get("message", "").lower(),
              "the error names the lexical index as the cause")
        # Everything else is unaffected, and the index costs no RAM.
        r = srv.req({"operation": "search", "tags": ["note"], "top_k": 3})
        check(len(r.get("records", [])) == 1, "tag search unaffected")
        s = srv.req({"operation": "stats"})
        check(s["indexes"]["lexical_terms"] == 0 and s["memory"]["lexical_bytes"] == 0,
              "disabled index reports zero terms and zero bytes")


def test_lexical_recovery(binary, port):
    print("[search: lexical index rebuilds from the log on restart]")
    datadir = tempfile.mkdtemp(prefix="aegis_lexrec_")
    with Server(binary, port, phase=4, datadir=datadir) as srv:
        keep = srv.req({"operation": "insert", "type": "semantic",
                        "data": "quotas via --tenant-max-records",
                        "tags": ["note"]})["record"]["id"]
        gone = srv.req({"operation": "insert", "type": "semantic",
                        "data": "this mentions blue-green deploys",
                        "tags": ["note"]})["record"]["id"]
        changed = srv.req({"operation": "insert", "type": "semantic",
                           "data": "original wording about zebras",
                           "tags": ["note"]})["record"]["id"]
        srv.req({"operation": "delete", "id": gone})
        srv.req({"operation": "update", "id": changed,
                 "data": "revised wording about giraffes"})
        srv.graceful_stop()

    # The index is derived and never checkpointed, so recovery must rebuild it
    # from the log — including honouring tombstones and superseded versions.
    with Server(binary, port, phase=4, datadir=datadir) as srv:
        r = srv.req({"operation": "search", "query": "--tenant-max-records",
                     "top_k": 3})
        check([rec["id"] for rec in r.get("records", [])] == [keep],
              "indexed term still matches after restart")
        r = srv.req({"operation": "search", "query": "blue-green", "top_k": 3})
        check(r.get("records") == [], "deleted record not reindexed on recovery")
        r = srv.req({"operation": "search", "query": "zebras", "top_k": 3})
        check(r.get("records") == [], "superseded text not reindexed on recovery")
        r = srv.req({"operation": "search", "query": "giraffes", "top_k": 3})
        check([rec["id"] for rec in r.get("records", [])] == [changed],
              "current version reindexed on recovery")
        s = srv.req({"operation": "stats"})
        check(s["indexes"]["lexical_docs"] == 2,
              "rebuilt index holds exactly the live documents")


def test_lexical_isolation(binary, port):
    print("[search: lexical results respect namespace isolation]")
    tokens = ["tok_a alice rw", "tok_b bob rw"]
    with Server(binary, port, phase=4, token_lines=tokens) as srv:
        srv.req({"operation": "insert", "type": "semantic", "token": "tok_a",
                 "data": "alice knows the --tenant-max-records value"})
        srv.req({"operation": "insert", "type": "semantic", "token": "tok_b",
                 "data": "bob also wrote --tenant-max-records somewhere"})

        # The lexical index is global, so the namespace filter must be what keeps
        # a tenant's text out of another tenant's results.
        ra = srv.req({"operation": "search", "token": "tok_a",
                      "query": "--tenant-max-records", "top_k": 10})
        check(len(ra["records"]) == 1 and "alice" in ra["records"][0]["data"],
              "alice's query returns only alice's record")
        rb = srv.req({"operation": "search", "token": "tok_b",
                      "query": "--tenant-max-records", "top_k": 10})
        check(len(rb["records"]) == 1 and "bob" in rb["records"][0]["data"],
              "bob's query returns only bob's record")
        # A spoofed agent_id cannot widen a namespaced token's view.
        rs = srv.req({"operation": "search", "token": "tok_a", "agent_id": "bob",
                      "query": "--tenant-max-records", "top_k": 10})
        check(len(rs["records"]) == 1 and "alice" in rs["records"][0]["data"],
              "a spoofed agent_id does not cross tenants")


def test_usage_feedback(binary, port):
    print("[usage feedback: recall counts on records]")
    with Server(binary, port, phase=4) as srv:
        a = srv.req({"operation": "insert", "type": "semantic", "tags": ["n"],
                     "data": "alpha about deployment"})["record"]["id"]
        b = srv.req({"operation": "insert", "type": "semantic", "tags": ["n"],
                     "data": "beta about watering plants"})["record"]["id"]

        # A fresh record is tracked with a real zero — distinguishable from an
        # untracked one, which omits the field entirely.
        rec = srv.req({"operation": "get", "id": a})["record"]
        check(rec.get("recall_count") == 0, "a fresh record reports zero recalls")
        check("last_recalled" not in rec,
              "never-recalled record omits last_recalled")

        for _ in range(3):
            srv.req({"operation": "search", "query": "deployment", "top_k": 5})
        rec = srv.req({"operation": "get", "id": a})["record"]
        check(rec.get("recall_count") == 3, "search hits increment the count")
        check(rec.get("last_recalled", 0) > 0, "last_recalled is set")
        check(srv.req({"operation": "get", "id": b})["record"]["recall_count"] == 0,
              "a record the search did not return is untouched")

        # `get` reports but must not increment: walking ids with a tool would
        # otherwise inflate every record's apparent value.
        for _ in range(3):
            srv.req({"operation": "get", "id": a})
        check(srv.req({"operation": "get", "id": a})["record"]["recall_count"] == 3,
              "get reports usage without incrementing it")

        # Opt-out, which is how a browser of memories avoids polluting the signal.
        srv.req({"operation": "search", "query": "deployment", "top_k": 5,
                 "track_usage": False})
        check(srv.req({"operation": "get", "id": a})["record"]["recall_count"] == 3,
              "track_usage:false does not increment")

        # Counters ride along on search results too.
        recs = srv.req({"operation": "search", "query": "deployment",
                        "top_k": 5})["records"]
        hit = [r for r in recs if r["id"] == a][0]
        check(hit.get("recall_count") == 4,
              "search results carry the count, including the current recall")

        s = srv.req({"operation": "stats"})
        check(s["indexes"]["usage_tracked"] == 2, "stats reports tracked records")
        check(s["memory"]["usage_bytes"] > 0, "stats reports usage index bytes")


def test_usage_feedback_forget(binary, port):
    print("[usage feedback: forget weighs recall history]")
    with Server(binary, port, phase=4) as srv:
        # Two records identical but for their recall history.
        used = srv.req({"operation": "insert", "type": "episodic",
                        "importance": 0.4,
                        "data": "widget rotation notes"})["record"]["id"]
        cold = srv.req({"operation": "insert", "type": "episodic",
                        "importance": 0.4,
                        "data": "sprocket alignment notes"})["record"]["id"]
        srv.req({"operation": "search", "query": "widget rotation", "top_k": 3})

        # min_retention above importance: both would go on importance alone, and
        # only the recall boost can save one of them.
        base = {"operation": "forget", "type": "episodic",
                "half_life_ms": 1000000, "min_retention": 0.5, "dry_run": True}
        r = srv.req(base)
        check(r["scanned"] == 2 and r["forgotten"] == 1,
              "the recalled record is spared, the unused one is not")
        check(r.get("usage_weight") == 1, "forget echoes the usage weight")

        r0 = srv.req({**base, "usage_weight": 0})
        check(r0["forgotten"] == 2,
              "usage_weight:0 reproduces the pre-feature scoring exactly")

        r2 = srv.req({"operation": "forget", "usage_weight": -1})
        check(r2.get("ok") is False, "a negative usage_weight is rejected")

        # For real this time.
        srv.req({k: v for k, v in base.items() if k != "dry_run"})
        check(srv.req({"operation": "get", "id": used}).get("ok") is True,
              "the recalled record survives a real forget")
        check(srv.req({"operation": "get", "id": cold})["error"]["code"]
              == "NOT_FOUND", "the unused record is forgotten")


def test_usage_feedback_persists(binary, port):
    print("[usage feedback: counters survive a restart]")
    datadir = tempfile.mkdtemp(prefix="aegis_usage_")
    with Server(binary, port, phase=4, datadir=datadir) as srv:
        rid = srv.req({"operation": "insert", "type": "semantic", "tags": ["n"],
                       "data": "alpha about deployment"})["record"]["id"]
        gone = srv.req({"operation": "insert", "type": "semantic", "tags": ["n"],
                        "data": "gamma about deployment"})["record"]["id"]
        for _ in range(4):
            srv.req({"operation": "search", "query": "deployment", "top_k": 5})
        srv.req({"operation": "delete", "id": gone})
        srv.graceful_stop()  # clean shutdown writes the checkpoint

    # Unlike every other index this one cannot be rebuilt from the log, so the
    # checkpoint is its only durability.
    with Server(binary, port, phase=4, datadir=datadir) as srv:
        rec = srv.req({"operation": "get", "id": rid})["record"]
        check(rec.get("recall_count") == 4, "recall counts survive a restart")
        check(rec.get("last_recalled", 0) > 0, "last_recalled survives a restart")
        s = srv.req({"operation": "stats"})
        check(s["indexes"]["usage_tracked"] == 1,
              "a record deleted before the restart is not resurrected")


def test_usage_feedback_disabled(binary, port):
    print("[usage feedback: --no-usage-feedback opt-out]")
    with Server(binary, port, phase=4,
                extra_args=["--no-usage-feedback"]) as srv:
        rid = srv.req({"operation": "insert", "type": "episodic",
                       "importance": 0.4, "tags": ["n"],
                       "data": "widget rotation notes"})["record"]["id"]
        srv.req({"operation": "search", "query": "widget", "top_k": 5})

        # Absent, not zero: the server keeps no counters at all, and a client can
        # tell that apart from "tracked but never recalled".
        rec = srv.req({"operation": "get", "id": rid})["record"]
        check("recall_count" not in rec, "no counters reported when disabled")
        s = srv.req({"operation": "stats"})
        check(s["indexes"]["usage_tracked"] == 0 and s["memory"]["usage_bytes"] == 0,
              "disabled index reports zero tracked and zero bytes")

        # forget still works and scores as it did before the feature.
        r = srv.req({"operation": "forget", "type": "episodic",
                     "half_life_ms": 1000000, "min_retention": 0.5,
                     "dry_run": True})
        check(r.get("ok") is True and r["forgotten"] == 1,
              "forget falls back to importance x recency alone")


def test_recall_latency_histogram(binary, port):
    print("[stats: recall-latency histogram (ROADMAP 3.3)]")
    with Server(binary, port, phase=4) as srv:
        # Absent before the first search, so a fresh server does not report a
        # misleading all-zero distribution.
        m = srv.req({"operation": "stats"})["metrics"]
        check("recall_latency" not in m,
              "no histogram before the first search")

        srv.req({"operation": "insert", "type": "semantic", "data": "a memory",
                 "tags": ["t"]})
        for _ in range(20):
            srv.req({"operation": "search", "tags": ["t"], "top_k": 1})
        # Other operations must not be counted — this is recall latency, not
        # request latency (dispatch_micros already covers everything).
        for _ in range(5):
            srv.req({"operation": "get", "id": 1})

        rl = srv.req({"operation": "stats"})["metrics"].get("recall_latency")
        check(isinstance(rl, dict), "stats reports recall_latency")
        check(rl.get("count") == 20, "only search observations are counted")
        for f in ("micros_total", "mean_micros", "p50_micros", "p95_micros",
                  "p99_micros", "buckets"):
            check(f in rl, f"recall_latency reports {f}")

        b = rl["buckets"]
        check("+Inf" in b, "histogram has an overflow bucket")
        check(b["+Inf"] == rl["count"], "+Inf bucket equals the count")
        finite = [k for k in b if k != "+Inf"]
        check(finite == sorted(finite, key=float),
              "finite buckets are emitted in ascending bound order")
        vals = [b[k] for k in finite] + [b["+Inf"]]
        check(vals == sorted(vals),
              "bucket counts are cumulative (Prometheus le semantics)")
        check(all(v <= rl["count"] for v in vals),
              "no bucket exceeds the observation count")

        # Percentiles must be ordered and inside the observed range.
        check(rl["p50_micros"] <= rl["p95_micros"] <= rl["p99_micros"],
              "percentile estimates are ordered p50 <= p95 <= p99")
        check(rl["mean_micros"] > 0, "mean latency is positive")
        check(abs(rl["mean_micros"] - rl["micros_total"] / rl["count"]) < 1e-6,
              "mean == micros_total / count")

        # The histogram is cumulative like every other counter: it keeps
        # accumulating rather than resetting per scrape.
        for _ in range(5):
            srv.req({"operation": "search", "tags": ["t"], "top_k": 1})
        rl2 = srv.req({"operation": "stats"})["metrics"]["recall_latency"]
        check(rl2["count"] == 25, "observations accumulate across scrapes")
        check(rl2["micros_total"] >= rl["micros_total"],
              "summed latency is monotonic")


def test_replication_encrypted(binary, port):
    print("[read replica: encrypted primary -> encrypted replica (shared key)]")
    repl_port = port + 1
    replica_port = port + 2
    tok = "repl-secret"
    keydir = tempfile.mkdtemp(prefix="aegis_replenc_")
    keyfile = os.path.join(keydir, "key.hex")
    with open(keyfile, "w") as fh:
        fh.write("cafebabecafebabecafebabecafebabe"
                 "cafebabecafebabecafebabecafebabe\n")
    badkey = os.path.join(keydir, "bad.hex")
    with open(badkey, "w") as fh:
        fh.write("22" * 32 + "\n")
    enc = ["--encryption-key-file", keyfile]

    with Server(binary, port, extra_args=[
            "--replication-port", str(repl_port),
            "--replication-token", tok] + enc) as primary:
        with Server(binary, replica_port, extra_args=[
                "--replicate-from", f"127.0.0.1:{repl_port}",
                "--replication-token", tok] + enc) as replica:
            r = primary.req({"operation": "insert", "type": "semantic",
                             "tags": ["e"], "data": "encrypted-replica-data"})
            check(r.get("ok") is True, "primary (encrypted) insert ok")
            rid = r["record"]["id"]
            got = None
            for _ in range(60):
                got = replica.req({"operation": "get", "id": rid})
                if got.get("ok"):
                    break
                time.sleep(0.1)
            check(got.get("ok") is True
                  and got["record"]["data"] == "encrypted-replica-data",
                  "record replicated to the encrypted replica")

        # both on-disk logs are ciphertext (no plaintext marker)
        for who in ("primary", "replica"):
            dd = primary.datadir if who == "primary" else replica.datadir
            with open(os.path.join(dd, "memory.log"), "rb") as fh:
                check(b"encrypted-replica-data" not in fh.read(),
                      f"{who} log is ciphertext on disk")

        # a replica with the WRONG key is rejected and never converges
        with Server(binary, replica_port + 10, extra_args=[
                "--replicate-from", f"127.0.0.1:{repl_port}",
                "--replication-token", tok,
                "--encryption-key-file", badkey]) as bad:
            converged = False
            for _ in range(15):
                if bad.req({"operation": "get", "id": rid}).get("ok"):
                    converged = True
                    break
                time.sleep(0.1)
            check(not converged, "wrong-key replica is rejected, does not converge")


def test_replication(binary, port):
    print("[read replica: log shipping]")
    repl_port = port + 1      # primary's replication stream port
    replica_port = port + 2   # the replica's own client port
    tok = "repl-secret"
    with Server(binary, port, extra_args=[
            "--replication-port", str(repl_port),
            "--replication-token", tok]) as primary:
        with Server(binary, replica_port, extra_args=[
                "--replicate-from", f"127.0.0.1:{repl_port}",
                "--replication-token", tok]) as replica:

            def wait_replica(payload, want, tries=60):
                # poll the replica until it converges (async replication)
                for _ in range(tries):
                    r = replica.req(payload)
                    if want(r):
                        return r
                    time.sleep(0.1)
                return r

            # write on the primary -> appears on the replica
            r = primary.req({"operation": "insert", "type": "semantic",
                             "tags": ["repl"], "data": "hello-replica"})
            check(r.get("ok") is True, "primary insert ok")
            rid = r["record"]["id"]
            r = wait_replica({"operation": "get", "id": rid},
                             lambda x: x.get("ok") is True)
            check(r.get("ok") is True and r["record"]["data"] == "hello-replica",
                  "record replicated to the replica")

            # tag search works on the replica (secondary indexes are maintained)
            r = replica.req({"operation": "search", "tags": ["repl"]})
            check(any(x["id"] == rid for x in r.get("records", [])),
                  "tag search on replica finds the record")

            # the replica refuses writes
            r = replica.req({"operation": "insert", "type": "episodic",
                             "data": "nope"})
            check(r.get("ok") is False and r["error"]["code"] == "READ_ONLY",
                  "write to replica -> READ_ONLY")

            # update on the primary re-indexes on the replica (old tag drops)
            primary.req({"operation": "update", "id": rid, "data": "v2",
                         "tags": ["repl2"]})
            r = wait_replica({"operation": "get", "id": rid},
                             lambda x: x.get("record", {}).get("data") == "v2")
            check(r.get("record", {}).get("data") == "v2", "update replicated")
            r = replica.req({"operation": "search", "tags": ["repl"]})
            check(not r.get("records"), "old tag dropped on replica after update")

            # delete on the primary propagates
            primary.req({"operation": "delete", "id": rid})
            r = wait_replica({"operation": "get", "id": rid},
                             lambda x: x.get("ok") is False)
            check(r.get("ok") is False and r["error"]["code"] == "NOT_FOUND",
                  "delete replicated -> NOT_FOUND on replica")

            # stats expose the replication posture
            st = replica.req({"operation": "stats"})
            check(st.get("replication", {}).get("role") == "replica",
                  "replica stats report role=replica")
            st = primary.req({"operation": "stats"})
            check(st.get("replication", {}).get("role") == "primary" and
                  st["replication"]["replicas"] >= 1,
                  "primary stats report a connected replica")


def test_replication_preauth(binary, port):
    print("[read replica: pre-auth handshake is bounded and slot-gated]")
    repl_port = port + 1
    handshake_timeout = 5  # mirrors HANDSHAKE_TIMEOUT_SEC in replication.c
    tok = "repl-secret"
    with Server(binary, port, extra_args=[
            "--replication-port", str(repl_port),
            "--replication-token", tok]) as primary:

        # a wrong token is rejected with ok:false before any log is streamed
        c = socket.create_connection(("127.0.0.1", repl_port), timeout=5)
        c.sendall(b'{"token":"wrong","from_offset":0,"generation":0}\n')
        c.settimeout(5)
        data = c.recv(256)
        check(b'"ok":false' in data, "bad replication token -> ok:false, no stream")
        c.close()

        # connections that never complete the handshake must NOT occupy a replica
        # slot — the fix counts them as pending until the token is verified.
        silent = [socket.create_connection(("127.0.0.1", repl_port), timeout=5)
                  for _ in range(4)]
        time.sleep(0.5)
        st = primary.req({"operation": "stats"})
        check(st.get("replication", {}).get("replicas", -1) == 0,
              "un-authenticated connections are not counted as replicas")

        # and such a connection is dropped within the handshake bound, not held
        # open indefinitely (slow-loris).
        silent[0].settimeout(handshake_timeout + 4)
        start = time.monotonic()
        try:
            closed = silent[0].recv(64) == b""  # server close -> EOF
        except socket.timeout:
            closed = False
        elapsed = time.monotonic() - start
        check(closed and elapsed < handshake_timeout + 3,
              "silent pre-auth connection dropped within the handshake bound")
        for s in silent:
            s.close()


def test_token_admin(binary, port):
    print("[runtime token administration]")
    tokfile = None  # Server writes it under its datadir
    with Server(binary, port, token_lines=["adm-key"]) as srv:  # one global admin
        admin = {"token": "adm-key"}

        # list: the admin token is present, exposed by fingerprint (no secret)
        r = srv.req({"operation": "token_list", **admin})
        check(r.get("ok") is True and len(r.get("tokens", [])) == 1,
              "token_list shows the configured admin token")
        check("id" in r["tokens"][0] and r["tokens"][0]["scope"] == "admin",
              "listed token has a fingerprint id + scope, no secret")

        # non-admin / no token cannot administer
        r = srv.req({"operation": "token_list"})
        check(r.get("ok") is False and r["error"]["code"] == "UNAUTHORIZED",
              "token_list without a token -> UNAUTHORIZED")

        # add a namespaced rw token (server mints the secret)
        r = srv.req({"operation": "token_add", "namespace": "acme",
                     "scope": "rw", **admin})
        check(r.get("ok") is True and r.get("token") and r.get("id"),
              "token_add mints a secret + returns its id")
        acme_tok = r["token"]
        acme_id = r["id"]

        # the new token works, scoped to its namespace
        r = srv.req({"operation": "insert", "type": "episodic", "data": "hi",
                     "token": acme_tok})
        check(r.get("ok") is True, "minted token can write in its namespace")
        rid = r["record"]["id"]

        # a namespaced token cannot administer tokens
        r = srv.req({"operation": "token_add", "namespace": "x", "scope": "rw",
                     "token": acme_tok})
        check(r.get("ok") is False and r["error"]["code"] == "FORBIDDEN",
              "namespaced token cannot add tokens -> FORBIDDEN")

        # a namespace with a newline/space must be refused: the token file is a
        # space/newline-delimited format, so an embedded newline could inject a
        # bare-token (= global admin) line that survives a reload.
        for bad in ("evil rw\nsha256$deadbeef", "has space", "tab\there"):
            r = srv.req({"operation": "token_add", "namespace": bad,
                         "scope": "rw", **admin})
            check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
                  "token_add rejects a namespace with whitespace/newline")
        # the injection attempt left exactly the admin + acme tokens (no extra
        # line snuck into the token set)
        r = srv.req({"operation": "token_list", **admin})
        check(len(r.get("tokens", [])) == 2,
              "rejected token_add did not add any token")

        # revoke it -> it stops authenticating immediately (no restart)
        r = srv.req({"operation": "token_revoke", "id": acme_id, **admin})
        check(r.get("ok") is True and r.get("revoked") is True,
              "token_revoke removes the token")
        r = srv.req({"operation": "get", "id": rid, "token": acme_tok})
        check(r.get("ok") is False and r["error"]["code"] == "UNAUTHORIZED",
              "revoked token no longer authenticates")

        # revoking an unknown id -> NOT_FOUND
        r = srv.req({"operation": "token_revoke", "id": "deadbeef0000", **admin})
        check(r.get("ok") is False and r["error"]["code"] == "NOT_FOUND",
              "revoking an unknown id -> NOT_FOUND")

        # changes persisted to the token file (admin + none for acme now)
        tokfile = os.path.join(srv.datadir, "tokens")
        with open(tokfile) as fh:
            body = fh.read()
        check("acme" not in body, "revoked token removed from the token file")
        check(body.count("sha256$") == 1, "token file rewritten (hashed) with 1 token")
        # the rewritten token file is owner-only (created 0600, no world-readable window)
        check((os.stat(tokfile).st_mode & 0o777) == 0o600,
              "persisted token file is mode 0600")


def test_tenant_quota(binary, port):
    print("[per-tenant storage quota]")
    tokens = ["adm", "acme-key acme rw", "beta-key beta rw"]
    with Server(binary, port, token_lines=tokens,
                extra_args=["--tenant-max-records", "3"]) as srv:
        acme = {"token": "acme-key"}
        # first 3 inserts for acme succeed
        for i in range(3):
            r = srv.req({"operation": "insert", "type": "episodic",
                         "data": f"a{i}", **acme})
            check(r.get("ok") is True, f"acme insert {i} within quota")
        # the 4th is rejected with QUOTA_EXCEEDED (still 3 live records)
        r = srv.req({"operation": "insert", "type": "episodic", "data": "a3", **acme})
        check(r.get("ok") is False and r["error"]["code"] == "QUOTA_EXCEEDED",
              "insert over record quota -> QUOTA_EXCEEDED")

        # a different tenant has its own quota (isolation)
        r = srv.req({"operation": "insert", "type": "episodic", "data": "b0",
                     "token": "beta-key"})
        check(r.get("ok") is True, "beta unaffected by acme's quota")

        # deleting frees a slot, so a subsequent insert fits again
        ids = [x["id"] for x in
               srv.req({"operation": "search", "top_k": 10, "start_time": 0,
                        "end_time": 9999999999999, **acme}).get("records", [])]
        d = srv.req({"operation": "delete", "id": ids[0], **acme})
        check(d.get("ok") is True, "acme delete frees a slot")
        r = srv.req({"operation": "insert", "type": "episodic", "data": "a3", **acme})
        check(r.get("ok") is True, "insert fits after freeing a slot")

        # admin stats reports per-tenant usage
        st = srv.req({"operation": "stats", "token": "adm"})
        tenants = {t["namespace"]: t for t in st.get("tenants", [])}
        check(tenants.get("acme", {}).get("records") == 3,
              "stats reports acme at its record cap")
        check(st.get("tenant_limits", {}).get("max_records") == 3,
              "stats reports the configured record limit")


def test_tenant_rate_limit(binary, port):
    print("[per-tenant rate limit]")
    tokens = ["adm", "acme-key acme rw"]
    # 5 req/s, burst 5: a quick burst of gets must eventually hit RATE_LIMITED
    with Server(binary, port, token_lines=tokens,
                extra_args=["--tenant-rate-qps", "5"]) as srv:
        acme = {"token": "acme-key"}
        limited = False
        for _ in range(30):
            r = srv.req({"operation": "get", "id": 1, **acme})
            code = (r.get("error") or {}).get("code")
            if code == "RATE_LIMITED":
                limited = True
                break
        check(limited, "a burst beyond the rate hits RATE_LIMITED")
        # ping is always exempt (health checks must never be rate-limited)
        check(srv.req({"operation": "ping"}).get("ok") is True,
              "ping is exempt from the rate limit")


def test_search_limits(binary, port):
    print("[search input limits]")
    with Server(binary, port, phase=4) as srv:  # default --embedding-dim 384
        r = srv.req({"operation": "search", "embedding": [0.0] * 385, "top_k": 5})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "oversized embedding -> INVALID_REQUEST")

        r = srv.req({"operation": "search",
                     "tags": [f"t{i}" for i in range(33)]})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "too many tags -> INVALID_REQUEST")

        # a huge top_k is clamped, not fatal
        r = srv.req({"operation": "search", "tags": ["x"], "top_k": 10**18})
        check(r.get("ok") is True, "huge top_k is clamped, not an error")


def test_query_scan_cap(binary, port):
    print("[query scan cap: broad search/count is bounded, count flags it]")
    # Tiny cap so a filterless scan is bounded to the most-recent 2 records.
    with Server(binary, port, phase=4,
                extra_args=["--query-scan-cap", "2"]) as srv:
        for i in range(5):
            srv.req({"operation": "insert", "type": "episodic",
                     "data": f"m{i}"})

        # unfiltered count is bounded to the cap and flags itself as capped
        r = srv.req({"operation": "count"})
        check(r.get("ok") is True and r.get("count") == 2 and r.get("capped") is True,
              "unfiltered count is capped at 2 and flagged")

        # unfiltered search is bounded too (still succeeds, not an error)
        r = srv.req({"operation": "search", "top_k": 100})
        check(r.get("ok") is True and len(r.get("records", [])) == 2,
              "unfiltered search bounded to the cap")

        # a selective tag filter is exact and never flagged
        srv.req({"operation": "insert", "type": "episodic",
                 "tags": ["only"], "data": "x"})
        r = srv.req({"operation": "count", "tags": ["only"]})
        check(r.get("ok") is True and r.get("count") == 1 and r.get("capped") is None,
              "tag-filtered count is exact and not capped")


def test_encryption_at_rest(binary, port):
    print("[encryption at rest: log sealed, survives restart, wrong key refused]")
    datadir = tempfile.mkdtemp(prefix="aegis_enc_")
    keyfile = os.path.join(datadir, "key.hex")
    with open(keyfile, "w") as fh:
        fh.write("00112233445566778899aabbccddeeff"
                 "00112233445566778899aabbccddeeff\n")  # 32 bytes hex
    marker = "TOP-SECRET-MEMORY-MARKER-42"
    enc_args = ["--encryption-key-file", keyfile]

    # write under encryption, then stop gracefully so a checkpoint is written
    with Server(binary, port, datadir=datadir, extra_args=enc_args) as srv:
        r = srv.req({"operation": "insert", "type": "semantic",
                     "tags": ["enc"], "data": marker})
        check(r.get("ok") is True, "insert into an encrypted log ok")
        rid = r["record"]["id"]
        r = srv.req({"operation": "get", "id": rid})
        check(r.get("ok") is True and r["record"]["data"] == marker,
              "read back within the same session")
        srv.graceful_stop()  # clean shutdown -> encrypted checkpoint on disk

    # neither the log nor the checkpoint may contain the plaintext marker
    with open(os.path.join(datadir, "memory.log"), "rb") as fh:
        blob = fh.read()
    check(marker.encode() not in blob, "plaintext marker absent from the log file")
    idx = os.path.join(datadir, "memory.index")
    if os.path.exists(idx):
        with open(idx, "rb") as fh:
            head = fh.read(4)
        check(head != b"AIDX", "checkpoint is encrypted (not the plaintext header)")

    # restart with the right key -> recovery decrypts and the record is present
    with Server(binary, port, datadir=datadir, extra_args=enc_args) as srv:
        r = srv.req({"operation": "get", "id": rid})
        check(r.get("ok") is True and r["record"]["data"] == marker,
              "record recovered after restart with the correct key")

    # restart with the WRONG key -> server refuses to start
    badkey = os.path.join(datadir, "bad.hex")
    with open(badkey, "w") as fh:
        fh.write("ff" * 32 + "\n")
    with Server(binary, port, datadir=datadir, expect_exit=True,
                extra_args=["--encryption-key-file", badkey]) as srv:
        check(srv.rc is not None and srv.rc != 0,
              "wrong key -> server exits nonzero, does not start")

    # no key at all against an encrypted dir -> also refused
    with Server(binary, port, datadir=datadir, expect_exit=True) as srv:
        check(srv.rc is not None and srv.rc != 0,
              "missing key on an encrypted dir -> server exits nonzero")


def test_encrypt_migrate(binary, port):
    print("[encrypt-migrate: plaintext dir -> encrypted, data survives]")
    datadir = tempfile.mkdtemp(prefix="aegis_mig_")
    keyfile = os.path.join(datadir, "key.hex")
    with open(keyfile, "w") as fh:
        fh.write("a1b2c3d4e5f60718293a4b5c6d7e8f90"
                 "a1b2c3d4e5f60718293a4b5c6d7e8f90\n")
    marker = "MIGRATE-ME-MARKER-88"

    # write plaintext, then stop cleanly
    with Server(binary, port, datadir=datadir) as srv:
        r = srv.req({"operation": "insert", "type": "episodic",
                     "tags": ["mig"], "data": marker})
        check(r.get("ok") is True, "plaintext insert ok")
        rid = r["record"]["id"]
        srv.graceful_stop()

    with open(os.path.join(datadir, "memory.log"), "rb") as fh:
        check(marker.encode() in fh.read(), "marker present in the plaintext log")

    # run the offline migration one-shot
    mig = subprocess.run(
        [binary, "--data-dir", datadir, "--encrypt-migrate",
         "--encryption-key-file", keyfile],
        capture_output=True, text=True)
    check(mig.returncode == 0, "--encrypt-migrate exits 0")

    with open(os.path.join(datadir, "memory.log"), "rb") as fh:
        check(marker.encode() not in fh.read(),
              "marker gone from the log after migration (now ciphertext)")

    # a second migrate is a no-op refusal (already encrypted)
    again = subprocess.run(
        [binary, "--data-dir", datadir, "--encrypt-migrate",
         "--encryption-key-file", keyfile],
        capture_output=True, text=True)
    check(again.returncode != 0, "re-migrating an encrypted dir is refused")

    # start with the key -> the migrated record is intact
    with Server(binary, port, datadir=datadir,
                extra_args=["--encryption-key-file", keyfile]) as srv:
        r = srv.req({"operation": "get", "id": rid})
        check(r.get("ok") is True and r["record"]["data"] == marker,
              "record survives the migration and reads back under the key")

    # starting the migrated dir WITHOUT the key is refused
    with Server(binary, port, datadir=datadir, expect_exit=True) as srv:
        check(srv.rc is not None and srv.rc != 0,
              "migrated dir without a key -> server exits nonzero")


def test_input_validation(binary, port):
    print("[input validation: agent_id, ttl_ms overflow, traverse depth]")
    with Server(binary, port, phase=4) as srv:
        # agent_id with a control char is rejected (would corrupt logs/token file)
        r = srv.req({"operation": "insert", "type": "episodic", "data": "x",
                     "agent_id": "bad\nid"})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "agent_id with a control char -> INVALID_REQUEST")
        # an over-long agent_id is rejected
        r = srv.req({"operation": "insert", "type": "episodic", "data": "x",
                     "agent_id": "a" * 200})
        check(r.get("ok") is False and r["error"]["code"] == "INVALID_REQUEST",
              "over-long agent_id -> INVALID_REQUEST")
        # a normal agent_id is accepted
        r = srv.req({"operation": "insert", "type": "episodic", "data": "x",
                     "agent_id": "team-alpha"})
        check(r.get("ok") is True, "well-formed agent_id accepted")

        # a huge ttl_ms saturates to "never" instead of wrapping into the past
        r = srv.req({"operation": "insert", "type": "episodic", "data": "ttl",
                     "ttl_ms": 18446744073709551615})
        check(r.get("ok") is True, "insert with a huge ttl_ms ok")
        rid = r["record"]["id"]
        r = srv.req({"operation": "get", "id": rid})
        check(r.get("ok") is True,
              "record with an overflowing ttl_ms is not immediately expired")

        # a huge traverse depth is clamped, not fatal (no int-cast garbage)
        r = srv.req({"operation": "insert", "type": "semantic", "data": "n"})
        nid = r["record"]["id"]
        r = srv.req({"operation": "traverse", "id": nid,
                     "depth": 9223372036854775807})
        check(r.get("ok") is True, "huge traverse depth is clamped, not an error")


def test_memory_cap(binary, port):
    print("[memory cap: inserts backpressure with MEMORY_LIMIT past --max-index-bytes]")
    # Cap just above the empty-index baseline (~41 KB); default --embedding-dim 384,
    # so a few dozen vector inserts cross it.
    with Server(binary, port, phase=4,
                extra_args=["--max-index-bytes", "55000"]) as srv:
        vec = [0.05] * 384

        def ins(i):
            return srv.req({"operation": "insert", "type": "semantic",
                            "tags": [f"m{i}"], "data": f"d{i}", "embedding": vec})

        check(ins(1).get("ok") is True, "insert under the cap succeeds")
        for i in range(2, 40):
            ins(i)
        total = srv.req({"operation": "stats"})["memory"]["index_bytes_total"]
        check(total > 55000, "index memory has crossed the cap")

        # the maintenance thread samples index memory every few seconds; wait for it
        time.sleep(3.5)
        r = ins(999)
        check(r.get("ok") is False and r["error"]["code"] == "MEMORY_LIMIT",
              "insert past the cap -> MEMORY_LIMIT")

        # backpressure is insert-only: working memory (bounded) and reads still work
        r = srv.req({"operation": "insert", "type": "working",
                     "session_id": "s", "data": "w"})
        check(r.get("ok") is True, "working-memory insert exempt from the cap")
        r = srv.req({"operation": "get", "id": 1})
        check(r.get("ok") is True, "reads still work when over the cap")


def test_phase_gating(binary, port):
    print("[phase 1: gating]")
    with Server(binary, port, phase=1) as srv:
        # ping + episodic insert + get are phase-1 operations
        check(srv.req({"operation": "ping"}).get("ok") is True, "ping available")
        r = srv.req({"operation": "insert", "type": "episodic", "data": "x"})
        check(r.get("ok") is True, "episodic insert available at phase 1")

        # delete is a phase-1 operation
        eid = r["record"]["id"]
        r = srv.req({"operation": "delete", "id": eid})
        check(r.get("ok") is True and r.get("deleted") is True,
              "delete available at phase 1")

        # search is phase 2+ -> NOT_READY
        r = srv.req({"operation": "search", "tags": ["x"], "top_k": 5})
        check(r.get("ok") is False and r["error"]["code"] == "NOT_READY",
              "search gated -> NOT_READY at phase 1")


def test_concurrency(binary, port):
    print("[concurrency: connections >> io-threads]")
    N = 24
    # Only 2 io-threads, but N persistent connections must all be served
    # concurrently. Under the old thread-per-connection model this capped at 2.
    with Server(binary, port, phase=4, io_threads=2) as srv:
        results = [False] * N

        def worker(idx):
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=5)
                s.settimeout(5)
                f = s.makefile("rwb")
                for j in range(5):  # several round-trips on a persistent conn
                    req = {"operation": "insert", "type": "episodic",
                           "tags": ["c"], "data": f"w{idx}-{j}"}
                    f.write((json.dumps(req) + "\n").encode())
                    f.flush()
                    if not json.loads(f.readline().decode()).get("ok"):
                        return
                f.write((json.dumps({"operation": "search", "tags": ["c"],
                                     "top_k": 3}) + "\n").encode())
                f.flush()
                if not json.loads(f.readline().decode()).get("ok"):
                    return
                s.close()
                results[idx] = True
            except OSError:
                results[idx] = False

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(N)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=20)
        served = sum(results)
        check(served == N,
              f"{served}/{N} persistent connections served concurrently by 2 io-threads")

        # Many idle connections must not starve a freshly active one.
        idles = []
        try:
            for _ in range(30):
                idles.append(socket.create_connection(("127.0.0.1", port), timeout=2))
            a = socket.create_connection(("127.0.0.1", port), timeout=2)
            a.settimeout(3)
            af = a.makefile("rwb")
            af.write(b'{"operation":"ping"}\n')
            af.flush()
            r = json.loads(af.readline().decode())
            check(r.get("ok") is True,
                  "active connection served while 30 idle connections are open")
            a.close()
        finally:
            for s in idles:
                s.close()


def test_framing(binary, port):
    """Line framing itself: everything above uses one request per connection with
    a clean trailing newline. Real clients pipeline, split writes across TCP
    segments, and send CRLF — the server must not depend on a request arriving in
    exactly one read()."""
    print("[framing: CRLF, pipelining, split writes, blank lines]")
    with Server(binary, port, phase=4) as srv:
        # CRLF line endings are tolerated (the \r must not reach the JSON parser).
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.settimeout(5)
            s.sendall(b'{"operation":"ping","request_id":"crlf"}\r\n')
            f = s.makefile("rwb")
            r = json.loads(f.readline().decode())
            check(r.get("ok") is True and r.get("request_id") == "crlf",
                  "CRLF-terminated request is handled")

        # Pipelining: several requests in ONE write must all be answered, in
        # order. This is the path where a response is staged while more input is
        # already buffered.
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.settimeout(10)
            n = 8
            batch = b"".join(
                (json.dumps({"operation": "ping", "request_id": f"p{i}"})
                 + "\n").encode() for i in range(n))
            s.sendall(batch)
            f = s.makefile("rwb")
            got = [json.loads(f.readline().decode()) for _ in range(n)]
            check(all(r.get("ok") for r in got),
                  f"all {n} pipelined requests answered")
            check([r.get("request_id") for r in got] == [f"p{i}" for i in range(n)],
                  "pipelined responses come back in request order")

        # A request split across many small writes (worst case: one byte at a
        # time) must be buffered until the newline arrives, not parsed early.
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.settimeout(10)
            payload = (json.dumps({"operation": "insert", "type": "episodic",
                                   "data": "split-write", "tags": ["frag"]})
                       + "\n").encode()
            for b in payload:
                s.sendall(bytes([b]))
            f = s.makefile("rwb")
            r = json.loads(f.readline().decode())
            check(r.get("ok") is True,
                  "request delivered one byte per write is handled")

        # A partial request with no newline yet must not be answered — and must
        # still complete once the rest arrives on the same connection.
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.settimeout(3)
            s.sendall(b'{"operation":"ping","request_id":"hal')
            s.settimeout(0.7)
            try:
                early = s.recv(4096)
            except socket.timeout:
                early = b""
            check(early == b"", "incomplete line is not answered early")
            s.settimeout(5)
            s.sendall(b'f"}\n')
            f = s.makefile("rwb")
            r = json.loads(f.readline().decode())
            check(r.get("ok") is True and r.get("request_id") == "half",
                  "the completed line is answered once its newline arrives")

        # Blank lines carry no request: they must be skipped silently (a cheap
        # client keepalive) and must not desynchronize the responses that follow.
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.settimeout(5)
            s.sendall(b'\n\n\r\n{"operation":"ping","request_id":"afterblank"}\n')
            f = s.makefile("rwb")
            r = json.loads(f.readline().decode())
            check(r.get("ok") is True and r.get("request_id") == "afterblank",
                  "blank lines are ignored without desynchronizing the stream")

        # A malformed line is an error response, not a dropped connection: the
        # same socket must keep serving afterwards.
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.settimeout(5)
            f = s.makefile("rwb")
            for bad in (b'not json at all\n', b'{"operation":\n', b'[]\n',
                        b'{"operation":"nosuchop"}\n'):
                s.sendall(bad)
                r = json.loads(f.readline().decode())
                check(r.get("ok") is False,
                      f"malformed input rejected: {bad[:20]!r}")
            s.sendall(b'{"operation":"ping","request_id":"survived"}\n')
            r = json.loads(f.readline().decode())
            check(r.get("ok") is True and r.get("request_id") == "survived",
                  "connection survives a run of malformed requests")

        # A long-lived connection must keep answering without leaking state.
        with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
            s.settimeout(10)
            f = s.makefile("rwb")
            ok = 0
            for i in range(200):
                f.write((json.dumps({"operation": "insert", "type": "episodic",
                                     "data": f"reuse-{i}"}) + "\n").encode())
                f.flush()
                if json.loads(f.readline().decode()).get("ok"):
                    ok += 1
            check(ok == 200, f"{ok}/200 sequential requests on one connection")


def test_connection_guards(binary, port):
    """The DoS guards on the client port: an over-long request line, the
    concurrent-connection cap, and idle reaping. These only trigger on abusive
    traffic, so nothing else in this suite reaches them."""
    print("[connection guards: oversized line, max-connections, idle timeout]")

    # max_payload + 4 KiB of envelope slack is the accepted line length, so a
    # 1 KiB payload limit rejects anything past ~5 KiB on one line.
    with Server(binary, port, phase=4,
                extra_args=["--max-payload", "1024"]) as srv:
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.settimeout(10)
            # No newline anywhere: the server must reject on the length alone
            # rather than buffering without bound.
            try:
                s.sendall(b"x" * 200000)
            except OSError:
                pass  # server may already have closed us; the response is below
            data = b""
            try:
                while not data.endswith(b"\n"):
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    data += chunk
            except OSError:
                pass
            r = json.loads(data.decode()) if data.strip() else {}
            check(r.get("ok") is False
                  and r.get("error", {}).get("code") == "PAYLOAD_TOO_LARGE",
                  "over-long request line -> PAYLOAD_TOO_LARGE")
            # And the connection is dropped afterwards, so the buffer is released.
            s.settimeout(5)
            try:
                rest = s.recv(4096)
            except OSError:
                rest = b""
            check(rest == b"", "connection is closed after the oversized line")

        # The server is still healthy for well-behaved clients afterwards.
        r = srv.req({"operation": "ping"})
        check(r.get("ok") is True, "server still serving after an oversized line")
        # A payload over the limit inside a well-framed line is a normal
        # per-request error (not a connection-level one).
        r = srv.req({"operation": "insert", "type": "episodic",
                     "data": "y" * 2048})
        check(r.get("ok") is False
              and r["error"]["code"] == "PAYLOAD_TOO_LARGE",
              "payload past --max-payload -> PAYLOAD_TOO_LARGE")

    # --max-connections: past the cap, new sockets are accepted then immediately
    # closed, and the already-open ones keep working.
    cap = 4
    with Server(binary, port + 1, phase=4, io_threads=1,
                extra_args=["--max-connections", str(cap)]) as srv:
        held = []
        try:
            for _ in range(cap):
                s = socket.create_connection(("127.0.0.1", port + 1), timeout=5)
                s.settimeout(5)
                # Complete a request so the connection is definitely registered.
                s.sendall(b'{"operation":"ping"}\n')
                s.makefile("rwb").readline()
                held.append(s)
            check(len(held) == cap, f"{cap} connections up to the cap are served")

            refused = 0
            for _ in range(3):
                try:
                    x = socket.create_connection(("127.0.0.1", port + 1),
                                                 timeout=5)
                    x.settimeout(5)
                    x.sendall(b'{"operation":"ping"}\n')
                    if x.recv(4096) == b"":
                        refused += 1  # accepted then closed without answering
                    x.close()
                except OSError:
                    refused += 1
            check(refused == 3, "connections past --max-connections are dropped")

            # The capped-out server still answers on its existing connections.
            held[0].sendall(b'{"operation":"ping","request_id":"held"}\n')
            r = json.loads(held[0].makefile("rwb").readline().decode())
            check(r.get("ok") is True,
                  "existing connections keep working while at the cap")
        finally:
            for s in held:
                s.close()

        # Once the held connections close, capacity frees up again.
        for _ in range(50):
            try:
                r = srv.req({"operation": "ping"})
                break
            except OSError:
                time.sleep(0.1)
        else:
            r = {}
        check(r.get("ok") is True,
              "capacity is reclaimed once connections close")

    # --idle-timeout-sec reaps a connection that moves no bytes (slow-loris),
    # while an active one is never reaped.
    with Server(binary, port + 2, phase=4,
                extra_args=["--idle-timeout-sec", "1"]) as srv:
        idle = socket.create_connection(("127.0.0.1", port + 2), timeout=5)
        idle.settimeout(15)
        reaped = False
        deadline = time.time() + 12
        while time.time() < deadline:
            try:
                if idle.recv(4096) == b"":
                    reaped = True
                    break
            except socket.timeout:
                break
            except OSError:
                reaped = True
                break
        idle.close()
        check(reaped, "an idle connection is reaped by --idle-timeout-sec")

        # A connection that keeps sending traffic across the timeout survives.
        with socket.create_connection(("127.0.0.1", port + 2), timeout=10) as s:
            s.settimeout(10)
            f = s.makefile("rwb")
            alive = True
            for _ in range(5):  # ~2.5 s of activity, past the 1 s timeout
                f.write(b'{"operation":"ping"}\n')
                f.flush()
                line = f.readline()
                if not line or not json.loads(line.decode()).get("ok"):
                    alive = False
                    break
                time.sleep(0.5)
            check(alive, "an active connection is not reaped mid-conversation")


def test_search_candidate_selection(binary, port):
    print("[search: max_importance ceiling + order=oldest (candidate selection)]")
    with Server(binary, port, phase=4) as srv:
        ids = []
        for imp in (0.1, 0.2, 0.9, 0.3, 0.95):
            r = srv.req({"operation": "insert", "type": "episodic",
                         "tags": ["c"], "data": f"m{imp}", "importance": imp})
            ids.append(r["record"]["id"])

        # Baseline: no ceiling -> all five come back (backward compatible).
        r = srv.req({"operation": "search", "type": "episodic",
                     "start_time": 0, "end_time": 9999999999999, "top_k": 100})
        check(r.get("ok") and r.get("total") == 5,
              "search without max_importance returns all records")

        # Ceiling filters out the high-importance records (0.9, 0.95).
        r = srv.req({"operation": "search", "type": "episodic",
                     "start_time": 0, "end_time": 9999999999999,
                     "max_importance": 0.5, "order": "oldest", "top_k": 100})
        recs = r.get("records", [])
        check(r.get("ok") and len(recs) == 3,
              "max_importance=0.5 keeps only the 3 low-importance records")
        check(all(rec["importance"] <= 0.5 + 1e-6 for rec in recs),
              "every returned record is at or below the importance ceiling")

        # order=oldest -> ascending by (created, id) == insertion order, and only
        # the eligible ids (0.1, 0.2, 0.3 -> ids[0], ids[1], ids[3]).
        got = [rec["id"] for rec in recs]
        check(got == sorted(got), "order=oldest returns records oldest-first")
        check(got == [ids[0], ids[1], ids[3]],
              "oldest-first yields the eligible records in insertion order")

        # The shared filter flows to count for free.
        n = srv.req({"operation": "count", "type": "episodic", "max_importance": 0.5})
        check(n.get("ok") and n.get("count") == 3,
              "count honors max_importance (shared filter struct)")


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/aegisdb"
    if not os.path.exists(binary):
        print(f"server binary not found: {binary}", file=sys.stderr)
        return 2
    # Use distinct high ports to avoid collisions across the two server runs.
    test_full_phase(binary, 19470)
    test_delete(binary, 19472)
    test_auth(binary, 19473)
    test_phase_gating(binary, 19471)
    test_stats(binary, 19474)
    test_multitenancy(binary, 19475)
    test_hashed_tokens(binary, 19476)
    test_cli(binary, 19514)  # uses port, +1
    test_search_limits(binary, 19478)
    test_query_scan_cap(binary, 19491)
    test_input_validation(binary, 19495)
    test_memory_cap(binary, 19496)
    test_search_candidate_selection(binary, 19497)
    test_encryption_at_rest(binary, 19492)
    test_encrypt_migrate(binary, 19493)
    test_concurrency(binary, 19479)
    test_framing(binary, 19505)
    test_connection_guards(binary, 19506)  # uses port, +1, +2
    test_bulk_ops(binary, 19480)
    test_consolidate(binary, 19481)
    test_traverse_kinds(binary, 19540)
    test_edge_index_maintenance(binary, 19541)
    test_edge_index_replica_parity(binary, 19511)  # uses port, +1, +2
    test_traverse_reverse(binary, 19543)
    test_traverse_reverse_disabled(binary, 19544)
    test_traverse_reverse_recovery(binary, 19545)
    test_traverse_reverse_isolation(binary, 19546)
    test_forget(binary, 19499)
    test_export_and_purge(binary, 19500)
    test_export_purge_isolation(binary, 19501)
    test_temporal(binary, 19502)
    test_temporal_isolation(binary, 19503)
    test_memory_quality_metrics(binary, 19504)
    test_multivector(binary, 19482)
    test_include_embeddings(binary, 19487)
    test_search_explain(binary, 19498)
    test_lexical_search(binary, 19530)
    test_lexical_hybrid(binary, 19531)
    test_lexical_disabled(binary, 19532)
    test_lexical_recovery(binary, 19533)
    test_lexical_isolation(binary, 19534)
    test_recall_latency_histogram(binary, 19535)
    test_usage_feedback(binary, 19536)
    test_usage_feedback_forget(binary, 19537)
    test_usage_feedback_persists(binary, 19538)
    test_usage_feedback_disabled(binary, 19539)
    test_tenant_quota(binary, 19488)
    test_tenant_rate_limit(binary, 19489)
    test_token_admin(binary, 19490)
    test_replication(binary, 19520)  # uses port, +1, +2
    test_replication_codec_gate(binary, 19509)  # uses port, +1
    test_typed_facts(binary, 19547)
    test_pattern_search(binary, 19552)
    test_pattern_search_disabled_and_isolated(binary, 19553)
    test_predicate_registry(binary, 19570)
    test_predicate_registry_refuses_to_start(binary, 19551)
    test_codec_gate_with_a_real_v3_frame(binary, 19568)  # uses port, +1
    test_typed_facts_replica_parity(binary, 19548)  # uses port, +1 (repl stream), +2 (replica)
    test_replication_encrypted(binary, 19554)  # uses port, +1, +2, +10
    test_replication_preauth(binary, 19523)  # uses port, +1 (repl stream)
    test_snapshot(binary, 19483)
    test_snapshot_admin_only(binary, 19485)
    test_restore(binary, 19516)  # uses port, +1
    test_encrypted_backup_restore(binary, 19518)  # uses port, +1

    print()
    if FAILURES:
        print(f"CONTRACT TESTS FAILED: {len(FAILURES)} failure(s)")
        return 1
    print("ALL CONTRACT TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())