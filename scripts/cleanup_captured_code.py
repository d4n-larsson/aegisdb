#!/usr/bin/env python3
"""Remove captured code / tool-output noise from an AegisDB namespace.

Early versions of the capture hook stored raw source lines (e.g.
`46\tcontent: ""; position: fixed;`) because salience markers matched as
substrings. This enumerates a subject's records via `export`, flags the ones that
look like code/tool-output using the SAME `looks_like_code()` the capture hook
now uses, and deletes them.

Safe by default: it only prints what it would delete. Pass --apply to delete.

    # 1) find your namespace (records the capture hook wrote under):
    python3 scripts/cleanup_captured_code.py --host 127.0.0.1 --port 9470

    # 2) preview the junk in that namespace (dry run):
    python3 scripts/cleanup_captured_code.py --agent-id my-project-a1b2c3

    # 3) delete it:
    python3 scripts/cleanup_captured_code.py --agent-id my-project-a1b2c3 --apply

Deleted records are tombstoned; run a compaction (or the `purge`/scheduled
compactor) afterward to reclaim the on-disk log space.
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import sys

# Reuse the real heuristic so the cleanup matches exactly what capture now skips.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "integrations", "claude-code"))
from aegis_mcp.capture import looks_like_code  # noqa: E402


class Client:
    def __init__(self, host, port, token):
        self.host, self.port, self.token = host, port, token

    def req(self, payload):
        if self.token:
            payload.setdefault("token", self.token)
        with socket.create_connection((self.host, self.port), timeout=10) as s:
            s.sendall((json.dumps(payload) + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
        return json.loads(data.decode())


def list_namespaces(cli):
    """Probe: which namespaces hold episodic records? (helps pick --agent-id)."""
    r = cli.req({"operation": "search", "type": "episodic", "start_time": 0,
                 "end_time": 9999999999999, "top_k": 200, "include_embeddings": False})
    if not r.get("ok"):
        return None
    seen = {}
    for rec in r.get("records", []):
        ns = rec.get("agent_id") or "(none)"
        seen[ns] = seen.get(ns, 0) + 1
    return seen


def scan(cli, agent_id, want_type):
    """Enumerate the namespace via export; return (all_count, junk_records)."""
    junk, total, after = [], 0, 0
    while True:
        r = cli.req({"operation": "export", "agent_id": agent_id, "after_id": after,
                     "limit": 500, "include_embeddings": False})
        if not r.get("ok"):
            raise SystemExit(f"export failed: {r.get('error')}")
        recs = r.get("records", [])
        for rec in recs:
            if want_type != "all" and rec.get("type") != want_type:
                continue
            total += 1
            if looks_like_code(rec.get("data") or ""):
                junk.append(rec)
        if not r.get("has_more"):
            break
        after = r["cursor"]
    return total, junk


def main():
    ap = argparse.ArgumentParser(description="Delete captured code/tool-output memories")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9470)
    ap.add_argument("--token", default=os.environ.get("AEGIS_AUTH_TOKEN"))
    ap.add_argument("--agent-id", default=None, help="namespace to clean (required to delete)")
    ap.add_argument("--type", default="episodic", choices=["episodic", "semantic", "working", "all"])
    ap.add_argument("--apply", action="store_true", help="actually delete (default: dry run)")
    args = ap.parse_args()

    cli = Client(args.host, args.port, args.token)

    if not args.agent_id:
        ns = list_namespaces(cli)
        if ns is None:
            raise SystemExit("could not reach the server (check --host/--port/--token)")
        print("Namespaces with episodic records (pass one as --agent-id):")
        for name, n in sorted(ns.items(), key=lambda kv: -kv[1]):
            print(f"  {n:>6}  {name}")
        print("\nThen re-run with --agent-id <namespace> to preview, and --apply to delete.")
        return 0

    total, junk = scan(cli, args.agent_id, args.type)
    print(f"namespace '{args.agent_id}': scanned {total} {args.type} record(s), "
          f"{len(junk)} look like code/tool-output\n")
    for rec in junk[:50]:
        snippet = " ".join((rec.get("data") or "").split())[:90]
        print(f"  #{rec['id']:<8} {snippet}")
    if len(junk) > 50:
        print(f"  … and {len(junk) - 50} more")
    if not junk:
        print("nothing to clean up.")
        return 0

    if not args.apply:
        print(f"\nDRY RUN — nothing deleted. Re-run with --apply to delete these {len(junk)}.")
        return 0

    deleted = 0
    for rec in junk:
        r = cli.req({"operation": "delete", "id": rec["id"]})
        if r.get("ok"):
            deleted += 1
        else:
            print(f"  ! delete #{rec['id']} failed: {r.get('error')}", file=sys.stderr)
    print(f"\ndeleted {deleted}/{len(junk)}. Run a compaction to reclaim log space "
          f"(e.g. wait for the scheduled compactor, or restart).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
