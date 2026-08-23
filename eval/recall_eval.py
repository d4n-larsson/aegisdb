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

    def __init__(self, binary, port, embedding_dim, phase=4, extra_args=None):
        self.binary = binary
        self.port = port
        self.embedding_dim = embedding_dim
        self.phase = phase
        self.extra_args = extra_args or []
        self.datadir = tempfile.mkdtemp(prefix="aegis_eval_")

    def __enter__(self):
        args = [self.binary, "--data-dir", self.datadir, "--port", str(self.port),
                "--phase", str(self.phase), "--embedding-dim",
                str(self.embedding_dim)] + self.extra_args
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


def run_query(srv, q, embed, max_k, retrieval="semantic"):
    """Rank a query's results. `retrieval` selects the path under test (ROADMAP
    4.1): "semantic" sends only the embedding, "lexical" only the query text, and
    "hybrid" both (which the server fuses by reciprocal rank)."""
    payload = {
        "operation": "search",
        "top_k": max_k,
        "include_embeddings": False,
    }
    if retrieval in ("semantic", "hybrid"):
        payload["embedding"] = embed(q["text"])
    if retrieval in ("lexical", "hybrid"):
        payload["query"] = q["text"]
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


def measure(srv, queries, embed, label_to_ids, ks, max_k, retrieval="semantic"):
    """Run every query and aggregate mean recall@k and MRR + per-query rows."""
    id_to_label = {i: lbl for lbl, ids in label_to_ids.items() for i in ids}
    per = []
    for q in queries:
        ranked = run_query(srv, q, embed, max_k, retrieval)
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


def run_lexical_eval(args, ds, embed, ks, max_k):
    """Compare the three retrieval paths on one corpus (ROADMAP 4.1).

    Seeds the dataset once, then measures semantic-only, lexical-only, and hybrid
    over the same records and queries. On an identifier-heavy dataset the semantic
    column is the gap this feature exists to close, and hybrid is the number that
    has to beat it — printed side by side so a scoring change is visible rather
    than argued about."""
    modes = ("semantic", "lexical", "hybrid")
    results = {}
    with Server(args.binary, args.port, ds.get("embedding_dim", 256)) as srv:
        label_to_ids = seed(srv, ds["memories"], embed)
        for mode in modes:
            mean, mrr, per = measure(srv, ds["queries"], embed, label_to_ids, ks,
                                     max_k, retrieval=mode)
            results[mode] = {"mean_recall": mean, "mrr": mrr, "per_query": per}

    gate_k = args.gate_recall_at or max_k
    sem = results["semantic"]["mean_recall"].get(gate_k, 0.0)
    hyb = results["hybrid"]["mean_recall"].get(gate_k, 0.0)

    # Which queries each mode answers, computed before reporting so the JSON and
    # human paths (and the gate) all agree.
    sem_per = {p["query"]: p for p in results["semantic"]["per_query"]}
    hyb_per = {p["query"]: p for p in results["hybrid"]["per_query"]}
    fixed = [q for q in sem_per if not sem_per[q]["hit"] and hyb_per[q]["hit"]]
    broke = [q for q in sem_per if sem_per[q]["hit"] and not hyb_per[q]["hit"]]

    if args.json:
        print(json.dumps({
            "dataset": ds["name"], "embedder": args.embedder,
            "n_queries": len(ds["queries"]), "mode": "lexical-comparison",
            "gate_k": gate_k, "semantic_recall": sem, "hybrid_recall": hyb,
            "hybrid_only_hits": fixed, "hybrid_regressions": broke,
            "results": results,
        }, indent=2))
    else:
        print(f"\nAegisDB retrieval-mode comparison — dataset '{ds['name']}', "
              f"embedder '{args.embedder}', {len(ds['memories'])} memories, "
              f"{len(ds['queries'])} queries\n")
        kcols = "  ".join(f"R@{k}" for k in ks)
        print(f"  {'mode':<10}  {kcols}  MRR")
        print(f"  {'-'*10}  {'-'*len(kcols)}  -----")
        for mode in modes:
            r = results[mode]
            rc = "  ".join(f"{r['mean_recall'][k]:>3.0%}" for k in ks)
            print(f"  {mode:<10}  {rc}  {r['mrr']:.3f}")

        # Per-query detail for the queries semantic-only cannot answer — the
        # concrete evidence, not just an aggregate.
        if fixed:
            print(f"\n  {len(fixed)} query(ies) hybrid answers that semantic-only "
                  f"misses entirely:")
            for q in fixed[:12]:
                print(f"    + {q[:66]}")
        if broke:
            print(f"\n  {len(broke)} REGRESSION(S) — semantic-only hit, hybrid misses:")
            for q in broke:
                print(f"    - {q[:66]}")
        print(f"\n  semantic R@{gate_k}={sem:.2%}   hybrid R@{gate_k}={hyb:.2%}   "
              f"delta={hyb - sem:+.2%}\n")

    # The roadmap's bar for 4.1: on an identifier-heavy set hybrid must not do
    # worse than semantic-only, and must not lose a query semantic-only answered.
    # Regressions are the interesting failure — fusion trading old wins for new
    # ones would look fine in the aggregate.
    failed = False
    if hyb < sem:
        print(f"GATE FAILED: hybrid recall@{gate_k} {hyb:.2%} < semantic-only "
              f"{sem:.2%}", file=sys.stderr)
        failed = True
    if broke:
        print(f"GATE FAILED: hybrid lost {len(broke)} query(ies) that "
              f"semantic-only answered: {broke}", file=sys.stderr)
        failed = True
    return 1 if failed else 0


def seed_multihop(srv, memories, embed):
    """Seed a dataset whose memories carry typed facts.

    Two passes, because a fact names its subject and object by *record id* and
    those ids do not exist until the entity records are written. The dataset
    refers to them by label; this is where labels become ids.
    """
    label_to_ids = {}
    order = []
    for m in memories:
        vec = embed(m["text"])
        payload = {
            "operation": "insert", "type": m.get("type", "semantic"),
            "data": m["text"], "tags": m.get("tags", []),
            "importance": m.get("importance", 0.5), "embedding": vec,
            "include_embeddings": False,
        }
        if m.get("fact"):
            order.append((m, payload))
            continue
        r = srv.req(payload)
        if not r.get("ok"):
            raise RuntimeError(f"insert failed for {m['label']}: {r}")
        label_to_ids.setdefault(m["label"], []).append(r["record"]["id"])

    for m, payload in order:
        f = dict(m["fact"])
        f["s"] = label_to_ids[f["s"]][0]
        if isinstance(f.get("o"), dict) and "label" in f["o"]:
            f["o"] = {"id": label_to_ids[f["o"]["label"]][0]}
        payload["fact"] = f
        r = srv.req(payload)
        if not r.get("ok"):
            raise RuntimeError(f"insert failed for {m['label']}: {r}")
        label_to_ids.setdefault(m["label"], []).append(r["record"]["id"])
    return label_to_ids


def await_fixpoint(srv, timeout=30.0, quiet_polls=4):
    """Wait until the inference job stops writing, and return what it derived.

    One pass draws only from the facts it can see, so a conclusion becomes the
    next pass's premise and a chain closes over several ticks. Waiting for the
    count to settle is the only honest way to say "inference has finished".
    """
    deadline = time.time() + timeout
    last, quiet = -1, 0
    while time.time() < deadline:
        n = srv.req({"operation": "stats"})["indexes"].get("derived", 0)
        quiet = quiet + 1 if n == last else 0
        last = n
        if quiet >= quiet_polls:
            return n
        time.sleep(0.25)
    return last


def run_symbolic_query(srv, q, label_to_ids, max_k):
    """Answer one query through the symbolic path: a pattern over typed facts,
    broadened through `is_a` so a question about a layer reaches a fact about one
    of its components."""
    pat = dict(q["pattern"])
    pat["s"] = label_to_ids[pat["s"]][0]
    r = srv.req({"operation": "search", "pattern": pat, "subsume": True,
                 "top_k": max_k, "include_embeddings": False})
    if not r.get("ok"):
        raise RuntimeError(f"symbolic search failed: {r}")
    return [rec["id"] for rec in r.get("records", [])]


def run_multihop_eval(args, ds, embed, ks, max_k):
    """The horizon's "done when" (ROADMAP 5.3): questions retrieval cannot answer.

    Every query here asks about a layer, and every answer record describes a leaf
    component without ever naming that layer — the two are connected only by a
    chain of `is_a` facts. So there is no record for similarity or BM25 to find:
    the answer does not live in any one of them. Retrieval scoring near zero is
    not a bug in the embedder, it is the property that makes the comparison
    meaningful, and the symbolic column is the claim under test.

    If retrieval scores well here the dataset is not multi-hop and the number
    proves nothing. The gate checks both directions for exactly that reason."""
    registry = None
    if ds.get("predicates"):
        fh = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        json.dump(ds["predicates"], fh)
        fh.close()
        registry = fh.name

    extra = ["--inference", "--inference-interval-sec", "1"]
    if registry:
        extra += ["--predicate-registry", registry]

    results = {}
    with Server(args.binary, args.port, ds.get("embedding_dim", 256),
                extra_args=extra) as srv:
        label_to_ids = seed_multihop(srv, ds["memories"], embed)
        derived = await_fixpoint(srv)
        for mode in ("semantic", "lexical", "hybrid"):
            mean, mrr, per = measure(srv, ds["queries"], embed, label_to_ids, ks,
                                     max_k, retrieval=mode)
            results[mode] = {"mean_recall": mean, "mrr": mrr, "per_query": per}

        per, hits = [], {}
        for q in ds["queries"]:
            ranked = run_symbolic_query(srv, q, label_to_ids, max_k)
            recall, rr = score(ranked, q["relevant"], label_to_ids, ks)
            hits[q["text"]] = recall.get(max_k, 0.0) > 0
            per.append({"query": q["text"], "recall": recall, "rr": rr,
                        "hit": recall.get(max_k, 0.0) > 0})
        n = len(per) or 1
        results["symbolic"] = {
            "mean_recall": {k: sum(p["recall"][k] for p in per) / n for k in ks},
            "mrr": sum(p["rr"] for p in per) / n,
            "per_query": per,
        }

    if registry:
        os.unlink(registry)

    gate_k = args.gate_recall_at or max_k
    sym = results["symbolic"]["mean_recall"].get(gate_k, 0.0)
    best_retrieval = max(results[m]["mean_recall"].get(gate_k, 0.0)
                         for m in ("semantic", "lexical", "hybrid"))

    if args.json:
        print(json.dumps({
            "dataset": ds["name"], "embedder": args.embedder,
            "n_queries": len(ds["queries"]), "mode": "multihop",
            "derived": derived, "gate_k": gate_k,
            "symbolic_recall": sym, "best_retrieval_recall": best_retrieval,
            "results": results,
        }, indent=2))
    else:
        print(f"\nAegisDB multi-hop eval — dataset '{ds['name']}', embedder "
              f"'{args.embedder}', {len(ds['memories'])} memories, "
              f"{len(ds['queries'])} queries, {derived} conclusion(s) derived\n")
        kcols = "  ".join(f"R@{k}" for k in ks)
        print(f"  {'path':<10}  {kcols}  MRR")
        print(f"  {'-'*10}  {'-'*len(kcols)}  -----")
        for mode in ("semantic", "lexical", "hybrid", "symbolic"):
            r = results[mode]
            rc = "  ".join(f"{r['mean_recall'][k]:>3.0%}" for k in ks)
            print(f"  {mode:<10}  {rc}  {r['mrr']:.3f}")
        missed = [p["query"] for p in results["symbolic"]["per_query"]
                  if not p["hit"]]
        if missed:
            print(f"\n  {len(missed)} query(ies) the symbolic path also misses:")
            for q in missed[:12]:
                print(f"    - {q[:66]}")
        print(f"\n  symbolic R@{gate_k}={sym:.2%}   best retrieval "
              f"R@{gate_k}={best_retrieval:.2%}   delta={sym - best_retrieval:+.2%}\n")

    # Both directions, because either failing means the number is meaningless.
    # A low symbolic score says the horizon does not deliver; a high retrieval
    # score says these questions were answerable all along and the dataset is
    # not testing what it claims to.
    failed = False
    if sym < args.gate_threshold:
        print(f"GATE FAILED: symbolic recall@{gate_k} {sym:.2%} < "
              f"{args.gate_threshold:.0%}", file=sys.stderr)
        failed = True
    if best_retrieval > args.max_retrieval:
        print(f"GATE FAILED: retrieval answers {best_retrieval:.2%} of these "
              f"queries (> {args.max_retrieval:.0%}); they are not multi-hop, "
              f"so the comparison proves nothing", file=sys.stderr)
        failed = True
    return 1 if failed else 0


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
    ap.add_argument("--lexical", action="store_true",
                    help="retrieval-mode comparison (ROADMAP 4.1): measure "
                         "semantic-only vs lexical-only vs hybrid over one corpus; "
                         "pair with --dataset eval/datasets/identifiers.json")
    ap.add_argument("--multihop", action="store_true",
                    help="multi-hop mode (ROADMAP 5.3): seed a corpus whose "
                         "answers live in no single record, let inference close "
                         "the chains, then compare retrieval against the "
                         "symbolic path; pair with --dataset "
                         "eval/datasets/multihop.json")
    ap.add_argument("--max-retrieval", type=float, default=0.25,
                    help="in --multihop mode, the most any retrieval path may "
                         "score before the dataset is judged not multi-hop")
    ap.add_argument("--retrieval", default="semantic",
                    choices=["semantic", "lexical", "hybrid"],
                    help="retrieval path for a normal (non---lexical) run")
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
    if args.lexical:
        return run_lexical_eval(args, ds, embed, ks, max_k)
    if args.multihop:
        return run_multihop_eval(args, ds, embed, ks, max_k)

    with Server(args.binary, args.port, dim) as srv:
        label_to_ids = seed(srv, ds["memories"], embed)
        mean_recall, mrr, per_query = measure(
            srv, ds["queries"], embed, label_to_ids, ks, max_k, args.retrieval)

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