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
import socket
import subprocess
import sys
import tempfile
import time

FAILURES = []


def check(cond, msg):
    if cond:
        print(f"  ok   - {msg}")
    else:
        print(f"  FAIL - {msg}")
        FAILURES.append(msg)


class Server:
    def __init__(self, binary, port, phase=4, auth_token=None, token_lines=None):
        self.binary = binary
        self.port = port
        self.phase = phase
        self.auth_token = auth_token
        self.token_lines = token_lines  # lines for --auth-token-file
        self.proc = None
        self.datadir = tempfile.mkdtemp(prefix="aegis_contract_")

    def __enter__(self):
        args = [self.binary, "--data-dir", self.datadir, "--port",
                str(self.port), "--phase", str(self.phase)]
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
        # Wait for the listener to accept connections.
        for _ in range(50):
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    return self
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("server did not start")

    def __exit__(self, *a):
        if self.proc:
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

    print()
    if FAILURES:
        print(f"CONTRACT TESTS FAILED: {len(FAILURES)} failure(s)")
        return 1
    print("ALL CONTRACT TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())