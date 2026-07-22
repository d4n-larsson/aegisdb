#!/usr/bin/env python3
"""Recall-quality eval harness for AegisDB (ROADMAP Horizon 1.1).

Makes memory retrieval *measurable*: seed a labelled corpus, run a query set, and
score whether the right memories surfaced (recall@k and MRR), with a per-query
report. This turns scoring/recall changes from guesswork into a number that moves
— the scoreboard every downstream memory-quality change (extraction, dedup,
decay, distillation) is tuned against.

The server does not compute embeddings, so the harness embeds both memories and
queries itself. The default embedder is a deterministic, dependency-free hashing
embedder (CI-runnable, reproducible); swap in a real one with
`--embedder command --embedder-cmd '<prog>'` (see embedders.py) for higher
fidelity.

Usage:
    python3 eval/recall_eval.py [path/to/aegisdb] [--dataset eval/datasets/coding_agent.json]
                                [--embedder hashing] [--k 1,3,5,10]
                                [--gate-recall-at 5] [--gate-threshold 0.8]

Exit code is 0 (report-only) unless a gate is set and not met.
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embedders import resolve_embedder  # noqa: E402


class Server:
    """Minimal server lifecycle + wire client (mirrors the contract test)."""

    def __init__(self, binary, port, embedding_dim, phase=4):
        self.binary = binary
        self.port = port
        self.embedding_dim = embedding_dim
        self.phase = phase
        self.datadir = tempfile.mkdtemp(prefix="aegis_eval_")

    def __enter__(self):
        args = [self.binary, "--data-dir", self.datadir, "--port", str(self.port),
                "--phase", str(self.phase), "--embedding-dim", str(self.embedding_dim)]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        for _ in range(50):
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    return self
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("server did not start")

    def __exit__(self, *a):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()

    def req(self, payload):
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as s:
            s.sendall((json.dumps(payload) + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
        return json.loads(data.decode())


def seed(srv, memories, embed):
    """Insert every memory; return {label: assigned_id}."""
    label_to_id = {}
    for m in memories:
        payload = {
            "operation": "insert",
            "type": m.get("type", "semantic"),
            "data": m["text"],
            "tags": m.get("tags", []),
            "importance": m.get("importance", 0.5),
            "embedding": embed(m["text"]),
            "include_embeddings": False,
        }
        r = srv.req(payload)
        if not r.get("ok"):
            raise RuntimeError(f"insert failed for {m['label']}: {r}")
        label_to_id[m["label"]] = r["record"]["id"]
    return label_to_id


def run_query(srv, q, embed, max_k):
    payload = {
        "operation": "search",
        "embedding": embed(q["text"]),
        "top_k": max_k,
        "include_embeddings": False,
    }
    if q.get("tags"):
        payload["tags"] = q["tags"]
        payload["match"] = q.get("match", "all")
    r = srv.req(payload)
    if not r.get("ok"):
        raise RuntimeError(f"search failed: {r}")
    return [rec["id"] for rec in r.get("records", [])]


def score(ranked_ids, relevant_ids, ks):
    """recall@k for each k, and reciprocal rank of the first relevant hit."""
    relevant = set(relevant_ids)
    recall = {}
    for k in ks:
        hits = sum(1 for rid in ranked_ids[:k] if rid in relevant)
        recall[k] = hits / len(relevant) if relevant else 0.0
    rr = 0.0
    for i, rid in enumerate(ranked_ids, start=1):
        if rid in relevant:
            rr = 1.0 / i
            break
    return recall, rr


def main():
    ap = argparse.ArgumentParser(description="AegisDB recall-quality eval")
    ap.add_argument("binary", nargs="?", default="./build/aegisdb")
    ap.add_argument("--dataset", default="eval/datasets/coding_agent.json")
    ap.add_argument("--embedder", default="hashing", choices=["hashing", "command"])
    ap.add_argument("--embedder-cmd", default=None)
    ap.add_argument("--port", type=int, default=9971)
    ap.add_argument("--k", default="1,3,5,10", help="comma-separated k values")
    ap.add_argument("--gate-recall-at", type=int, default=None,
                    help="if set, fail (exit 1) when mean recall@this-k < threshold")
    ap.add_argument("--gate-threshold", type=float, default=0.8)
    ap.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = ap.parse_args()

    ks = sorted(int(x) for x in args.k.split(","))
    max_k = max(ks)
    with open(args.dataset) as fh:
        ds = json.load(fh)
    dim = ds.get("embedding_dim", 256)
    embed = resolve_embedder(args.embedder, dim, args.embedder_cmd)

    with Server(args.binary, args.port, dim) as srv:
        label_to_id = seed(srv, ds["memories"], embed)
        per_query = []
        for q in ds["queries"]:
            relevant_ids = [label_to_id[l] for l in q["relevant"]]
            ranked = run_query(srv, q, embed, max_k)
            recall, rr = score(ranked, relevant_ids, ks)
            # top hit label for the human report
            id_to_label = {v: k for k, v in label_to_id.items()}
            top_label = id_to_label.get(ranked[0], "-") if ranked else "-"
            per_query.append({
                "query": q["text"], "recall": recall, "rr": rr,
                "top": top_label, "expected": q["relevant"],
                "hit": recall.get(max_k, 0.0) > 0,
            })

    n = len(per_query)
    mean_recall = {k: sum(pq["recall"][k] for pq in per_query) / n for k in ks}
    mrr = sum(pq["rr"] for pq in per_query) / n

    if args.json:
        print(json.dumps({
            "dataset": ds["name"], "embedder": args.embedder, "n_queries": n,
            "mean_recall": mean_recall, "mrr": mrr,
            "per_query": per_query,
        }, indent=2))
    else:
        print(f"\nAegisDB recall eval — dataset '{ds['name']}', embedder '{args.embedder}', "
              f"{len(ds['memories'])} memories, {n} queries\n")
        kcols = "  ".join(f"R@{k}" for k in ks)
        print(f"  {'query':<52}  {kcols}  RR   top")
        print(f"  {'-'*52}  {'-'*len(kcols)}  ---  ---")
        for pq in per_query:
            rc = "  ".join(f"{pq['recall'][k]:>3.0%}" for k in ks)
            mark = " " if pq["hit"] else "*"
            q = (pq["query"][:50] + "..") if len(pq["query"]) > 52 else pq["query"]
            print(f"{mark} {q:<52}  {rc}  {pq['rr']:.2f} {pq['top']}")
        print(f"\n  mean:  " + "  ".join(f"R@{k}={mean_recall[k]:.2%}" for k in ks) +
              f"   MRR={mrr:.3f}")
        misses = [pq for pq in per_query if not pq["hit"]]
        if misses:
            print(f"  {len(misses)} miss(es) marked with * (no relevant memory in top {max_k})")
        print()

    if args.gate_recall_at is not None:
        got = mean_recall.get(args.gate_recall_at)
        if got is None:
            print(f"gate: k={args.gate_recall_at} not in --k set", file=sys.stderr)
            return 2
        if got < args.gate_threshold:
            print(f"GATE FAILED: mean recall@{args.gate_recall_at} "
                  f"{got:.2%} < {args.gate_threshold:.2%}", file=sys.stderr)
            return 1
        print(f"gate OK: mean recall@{args.gate_recall_at} {got:.2%} "
              f">= {args.gate_threshold:.2%}")
    return 0


if __name__ == "__main__":
    sys.exit(main())