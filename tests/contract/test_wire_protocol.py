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
        ins(base, "a"); ins(near1, "b"); ins(near2, "c")
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

        # idempotent
        r = srv.req({"operation": "consolidate", "min_similarity": 0.95})
        check(r.get("ok") is True and r.get("merged") == 0,
              "second consolidate is a no-op")


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
    test_cli(binary, 19477)
    test_search_limits(binary, 19478)
    test_query_scan_cap(binary, 19491)
    test_encryption_at_rest(binary, 19492)
    test_concurrency(binary, 19479)
    test_bulk_ops(binary, 19480)
    test_consolidate(binary, 19481)
    test_multivector(binary, 19482)
    test_include_embeddings(binary, 19487)
    test_tenant_quota(binary, 19488)
    test_tenant_rate_limit(binary, 19489)
    test_token_admin(binary, 19490)
    test_replication(binary, 19520)  # uses port, +1 (repl stream), +2 (replica)
    test_replication_preauth(binary, 19523)  # uses port, +1 (repl stream)
    test_snapshot(binary, 19483)
    test_snapshot_admin_only(binary, 19485)
    test_restore(binary, 19486)

    print()
    if FAILURES:
        print(f"CONTRACT TESTS FAILED: {len(FAILURES)} failure(s)")
        return 1
    print("ALL CONTRACT TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())