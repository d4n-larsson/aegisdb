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


def seed(srv, memories, embed, dup_factor=1):
    """Insert every memory (dup_factor identical copies each, for the consolidation
    eval). Return {label: [assigned_ids]} — a list so a duplicated memory and its
    post-consolidation survivor both count as a hit for that label."""
    label_to_ids = {}
    for m in memories:
        vec = embed(m["text"])
        for _ in range(dup_factor):
            payload = {
                "operation": "insert",
                "type": m.get("type", "semantic"),
                "data": m["text"],
                "tags": m.get("tags", []),
                "importance": m.get("importance", 0.5),
                "embedding": vec,
                "include_embeddings": False,
            }
            r = srv.req(payload)
            if not r.get("ok"):
                raise RuntimeError(f"insert failed for {m['label']}: {r}")
            label_to_ids.setdefault(m["label"], []).append(r["record"]["id"])
    return label_to_ids


def record_count(srv):
    r = srv.req({"operation": "stats"})
    return r.get("records", 0)


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


def score(ranked_ids, relevant_labels, label_to_ids, ks):
    """recall@k over *labels* (a label hits if any of its ids — original or
    post-consolidation survivor — is in the top-k), plus the reciprocal rank of
    the first relevant-label id."""
    recall = {}
    for k in ks:
        topk = set(ranked_ids[:k])
        hit = sum(1 for lbl in relevant_labels
                  if any(i in topk for i in label_to_ids.get(lbl, [])))
        recall[k] = hit / len(relevant_labels) if relevant_labels else 0.0
    rel_ids = {i for lbl in relevant_labels for i in label_to_ids.get(lbl, [])}
    rr = 0.0
    for i, rid in enumerate(ranked_ids, start=1):
        if rid in rel_ids:
            rr = 1.0 / i
            break
    return recall, rr


def measure(srv, queries, embed, label_to_ids, ks, max_k):
    """Run every query and aggregate mean recall@k and MRR + per-query rows."""
    id_to_label = {i: lbl for lbl, ids in label_to_ids.items() for i in ids}
    per = []
    for q in queries:
        ranked = run_query(srv, q, embed, max_k)
        recall, rr = score(ranked, q["relevant"], label_to_ids, ks)
        per.append({
            "query": q["text"], "recall": recall, "rr": rr,
            "top": id_to_label.get(ranked[0], "-") if ranked else "-",
            "expected": q["relevant"], "hit": recall.get(max_k, 0.0) > 0,
        })
    n = len(per) or 1
    mean = {k: sum(p["recall"][k] for p in per) / n for k in ks}
    mrr = sum(p["rr"] for p in per) / n
    return mean, mrr, per


def run_consolidate_eval(args, ds, embed, ks, max_k):
    """Measure consolidation (ROADMAP 2.2): seed duplicate clusters, then check
    that `consolidate` collapses the corpus WITHOUT losing recall of the surviving
    fact. The whole point of dedup is a smaller corpus at equal answer quality."""
    dim = ds.get("embedding_dim", 256)
    with Server(args.binary, args.port, dim) as srv:
        label_to_ids = seed(srv, ds["memories"], embed, dup_factor=args.dup_factor)
        before_n = record_count(srv)
        before_recall, before_mrr, _ = measure(
            srv, ds["queries"], embed, label_to_ids, ks, max_k)

        r = srv.req({"operation": "consolidate", "min_similarity": args.min_similarity})
        if not r.get("ok"):
            print(f"consolidate failed: {r}", file=sys.stderr)
            return 2
        clusters, merged = r.get("clusters", 0), r.get("merged", 0)

        after_n = record_count(srv)
        after_recall, after_mrr, _ = measure(
            srv, ds["queries"], embed, label_to_ids, ks, max_k)

    unique = len(ds["memories"])
    if args.json:
        print(json.dumps({
            "mode": "consolidate", "dataset": ds["name"], "dup_factor": args.dup_factor,
            "min_similarity": args.min_similarity, "clusters": clusters, "merged": merged,
            "records_before": before_n, "records_after": after_n, "unique_memories": unique,
            "recall_before": before_recall, "recall_after": after_recall,
            "mrr_before": before_mrr, "mrr_after": after_mrr,
        }, indent=2))
    else:
        print(f"\nAegisDB consolidation eval — dataset '{ds['name']}', "
              f"{unique} unique memories × {args.dup_factor} copies, "
              f"min_similarity={args.min_similarity}\n")
        print(f"  records:  {before_n} → {after_n}  "
              f"({merged} merged in {clusters} clusters; "
              f"ideal ≈ {unique})")
        print(f"  {'':<10}" + "  ".join(f"R@{k}" for k in ks) + "   MRR")
        print(f"  before    " + "  ".join(f"{before_recall[k]:>3.0%}" for k in ks) +
              f"   {before_mrr:.3f}")
        print(f"  after     " + "  ".join(f"{after_recall[k]:>3.0%}" for k in ks) +
              f"   {after_mrr:.3f}")
        print()

    # Gate: the corpus must shrink and recall@max_k must not regress.
    shrank = after_n < before_n
    held = after_recall[max_k] >= before_recall[max_k] - 1e-9
    if not shrank:
        print(f"GATE FAILED: consolidation did not shrink the corpus "
              f"({before_n} → {after_n})", file=sys.stderr)
        return 1
    if not held:
        print(f"GATE FAILED: recall@{max_k} regressed "
              f"{before_recall[max_k]:.2%} → {after_recall[max_k]:.2%}", file=sys.stderr)
        return 1
    print(f"OK: corpus {before_n} → {after_n} (−{before_n - after_n}), "
          f"recall@{max_k} held at {after_recall[max_k]:.2%}")
    return 0


def run_decay_eval(args, ds, embed, ks, max_k):
    """Measure forgetting (ROADMAP 2.3): seed the curated facts, flood the corpus
    with low-value episodic 'noise', then check that `forget` ages out the noise
    (corpus plateaus) WITHOUT losing recall of the facts. The point of forgetting
    is a bounded corpus at equal answer quality."""
    dim = ds.get("embedding_dim", 256)
    with Server(args.binary, args.port, dim) as srv:
        label_to_ids = seed(srv, ds["memories"], embed)  # the facts (semantic)
        # inject low-importance episodic noise — the volume that should age out
        for i in range(args.noise):
            r = srv.req({"operation": "insert", "type": "episodic",
                         "data": f"transient event {i}", "importance": 0.01,
                         "tags": ["noise"], "embedding": embed(f"transient event {i}"),
                         "include_embeddings": False})
            if not r.get("ok"):
                raise RuntimeError(f"noise insert failed: {r}")
        before_n = record_count(srv)
        before_recall, before_mrr, _ = measure(
            srv, ds["queries"], embed, label_to_ids, ks, max_k)

        # forget low-value episodic records (semantic facts are protected by type)
        r = srv.req({"operation": "forget", "min_retention": args.min_retention})
        if not r.get("ok"):
            print(f"forget failed: {r}", file=sys.stderr)
            return 2
        scanned, forgotten = r.get("scanned", 0), r.get("forgotten", 0)

        after_n = record_count(srv)
        after_recall, after_mrr, _ = measure(
            srv, ds["queries"], embed, label_to_ids, ks, max_k)

    facts = len(ds["memories"])
    if args.json:
        print(json.dumps({
            "mode": "decay", "dataset": ds["name"], "facts": facts, "noise": args.noise,
            "min_retention": args.min_retention, "scanned": scanned, "forgotten": forgotten,
            "records_before": before_n, "records_after": after_n,
            "recall_before": before_recall, "recall_after": after_recall,
            "mrr_before": before_mrr, "mrr_after": after_mrr,
        }, indent=2))
    else:
        print(f"\nAegisDB forgetting eval — dataset '{ds['name']}', "
              f"{facts} facts + {args.noise} low-value episodic records, "
              f"min_retention={args.min_retention}\n")
        print(f"  records:  {before_n} → {after_n}  "
              f"({forgotten} forgotten of {scanned} episodic scanned; "
              f"ideal ≈ {facts})")
        print(f"  {'':<10}" + "  ".join(f"R@{k}" for k in ks) + "   MRR")
        print(f"  before    " + "  ".join(f"{before_recall[k]:>3.0%}" for k in ks) +
              f"   {before_mrr:.3f}")
        print(f"  after     " + "  ".join(f"{after_recall[k]:>3.0%}" for k in ks) +
              f"   {after_mrr:.3f}")
        print()

    shrank = after_n < before_n
    held = after_recall[max_k] >= before_recall[max_k] - 1e-9
    if not shrank:
        print(f"GATE FAILED: forgetting did not shrink the corpus "
              f"({before_n} → {after_n})", file=sys.stderr)
        return 1
    if not held:
        print(f"GATE FAILED: recall@{max_k} regressed "
              f"{before_recall[max_k]:.2%} → {after_recall[max_k]:.2%}", file=sys.stderr)
        return 1
    print(f"OK: corpus {before_n} → {after_n} (−{before_n - after_n}), "
          f"recall@{max_k} held at {after_recall[max_k]:.2%}")
    return 0


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
    ap.add_argument("--consolidate", action="store_true",
                    help="dedup mode: seed --dup-factor copies of each memory, then "
                         "measure recall + corpus size before and after a consolidate")
    ap.add_argument("--dup-factor", type=int, default=3,
                    help="copies of each memory to seed in --consolidate mode")
    ap.add_argument("--min-similarity", type=float, default=0.95,
                    help="cosine threshold passed to consolidate")
    ap.add_argument("--decay", action="store_true",
                    help="forgetting mode: seed the facts + --noise low-value episodic "
                         "records, then measure recall + corpus size before and after a forget")
    ap.add_argument("--noise", type=int, default=200,
                    help="low-importance episodic records to inject in --decay mode")
    ap.add_argument("--min-retention", type=float, default=0.05,
                    help="retention floor passed to forget (importance*recency)")
    args = ap.parse_args()

    ks = sorted(int(x) for x in args.k.split(","))
    max_k = max(ks)
    with open(args.dataset) as fh:
        ds = json.load(fh)
    dim = ds.get("embedding_dim", 256)
    embed = resolve_embedder(args.embedder, dim, args.embedder_cmd)

    if args.consolidate:
        return run_consolidate_eval(args, ds, embed, ks, max_k)
    if args.decay:
        return run_decay_eval(args, ds, embed, ks, max_k)

    with Server(args.binary, args.port, dim) as srv:
        label_to_ids = seed(srv, ds["memories"], embed)
        mean_recall, mrr, per_query = measure(
            srv, ds["queries"], embed, label_to_ids, ks, max_k)

    n = len(per_query)

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