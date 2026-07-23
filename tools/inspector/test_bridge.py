#!/usr/bin/env python3
"""Smoke test for the inspector bridge (ROADMAP 1.3).

Spawns a real aegisdb + the bridge, then exercises every endpoint the UI uses:
config, stats, browse search, the operation allowlist, embed, semantic search
with the per-hit explain breakdown, edit, and delete. Verifies the *plumbing*
(shapes/round-trips), not ranking quality — that is the embedder's job.

    python3 tools/inspector/test_bridge.py [path/to/aegisdb]
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "eval"))
from embedders import subword_embedder  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "aegisdb")
DIM, APORT, HPORT = 384, 9476, 8602
FAILS = []


def check(cond, msg):
    print(("  ok   - " if cond else "  FAIL - ") + msg)
    if not cond:
        FAILS.append(msg)


def wire(p):
    s = socket.create_connection(("127.0.0.1", APORT), timeout=3)
    s.sendall((json.dumps(p) + "\n").encode())
    d = b""
    while not d.endswith(b"\n"):
        c = s.recv(65536)
        if not c:
            break
        d += c
    s.close()
    return json.loads(d)


def http(path, payload=None):
    if payload is None:
        return json.loads(urllib.request.urlopen("http://127.0.0.1:%d%s" % (HPORT, path)).read())
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (HPORT, path),
                                 data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        return json.loads(urllib.request.urlopen(req).read())
    except urllib.error.HTTPError as e:
        return json.loads(e.read())


def check_identity_drift():
    """The inspector inlines the brand tokens (so it renders standalone); this
    guards them against drifting from the canonical source, site/aegis.css."""
    print("[inspector identity drift guard]")
    import re
    with open(os.path.join(HERE, "index.html")) as fh:
        page = fh.read()
    with open(os.path.join(ROOT, "site", "aegis.css")) as fh:
        css = fh.read()

    def token(text, name):
        m = re.search(r"--%s:\s*(#[0-9A-Fa-f]{3,6})" % re.escape(name), text)
        return m.group(1).upper() if m else None

    for name in ("ink", "brass", "paper", "line", "episodic", "semantic", "working"):
        a, b = token(page, name), token(css, name)
        check(a is not None and a == b,
              f"--{name} matches site/aegis.css ({a} == {b})")


def main():
    check_identity_drift()
    datadir = tempfile.mkdtemp(prefix="aegis_inspector_")
    srv = subprocess.Popen([BIN, "--data-dir", datadir, "--port", str(APORT),
                            "--phase", "4", "--embedding-dim", str(DIM)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    br = subprocess.Popen([sys.executable, os.path.join(HERE, "bridge.py"),
                           "--aegis-port", str(APORT), "--http-port", str(HPORT),
                           "--embedding-dim", str(DIM)],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", HPORT), timeout=0.2).close()
                socket.create_connection(("127.0.0.1", APORT), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.1)

        print("[inspector bridge]")
        emb = subword_embedder(DIM)
        ids = []
        for text, tags in [("Deploys go through make ship", ["deploy"]),
                           ("Migrations run with make migrate", ["database"])]:
            r = wire({"operation": "insert", "type": "semantic", "data": text,
                      "tags": tags, "importance": 0.7, "confidence": 1.0,
                      "embedding": emb(text)})
            ids.append(r["record"]["id"])

        cfg = http("/api/config")
        check(cfg.get("embedding_dim") == DIM and cfg.get("embedder") == "subword",
              "/api/config reports embedder + dim")
        page = urllib.request.urlopen("http://127.0.0.1:%d/" % HPORT).read().decode()
        check("Memory Inspector</title>" in page, "/ serves the UI")
        # Self-contained: tokens are inlined so it renders however it's opened.
        check("--brass:#C9A24B" in page and "--ink:#14110E" in page,
              "/ inlines the shared identity tokens (renders without the bridge)")

        st = http("/api/query", {"operation": "stats"})
        check(st.get("ok") and st["records"] == 2, "stats via bridge")

        br_res = http("/api/query", {"operation": "search", "start_time": 0,
                                     "end_time": 9999999999999, "top_k": 10,
                                     "include_embeddings": False})
        check(br_res.get("ok") and br_res["total"] == 2, "browse search via bridge")

        blocked = http("/api/query", {"operation": "token_list"})
        check("error" in blocked and "not allowed" in blocked["error"],
              "operation allowlist blocks token_list")

        e = http("/api/embed", {"text": "deploy to prod"})
        check(isinstance(e.get("embedding"), list) and len(e["embedding"]) == DIM,
              "embed returns a dim-length vector")

        sem = http("/api/query", {"operation": "search", "embedding": e["embedding"],
                                  "top_k": 2, "explain": True, "include_embeddings": False})
        check(sem.get("ok") and all("explain" in r for r in sem["records"]),
              "semantic search returns explain on every hit")
        ex = sem["records"][0]["explain"]
        check(all(k in ex for k in ("score", "similarity", "weight", "recency_factor")),
              "explain has the ranking components")

        up = http("/api/query", {"operation": "update", "id": ids[0],
                                 "data": "Deploys go through make ship (edited)",
                                 "importance": 0.9, "confidence": 1.0, "tags": ["deploy"]})
        check(up.get("ok"), "edit (update) via bridge")

        dl = http("/api/query", {"operation": "delete", "id": ids[1]})
        check(dl.get("ok"), "delete via bridge")
        after = http("/api/query", {"operation": "stats"})
        check(after["records"] == 1, "record count drops after delete")
    finally:
        br.kill(); srv.kill(); br.wait(); srv.wait()

    print()
    if FAILS:
        print("INSPECTOR BRIDGE TESTS FAILED: %d" % len(FAILS))
        return 1
    print("ALL INSPECTOR BRIDGE TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())