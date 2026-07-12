"""Background memory distillation — operator-scheduled (see
docs/summarization-design.md).

Selects clusters of aging, low-value episodic memories, summarizes each via the
configured SummaryProvider into one semantic fact, links that fact to its sources
(`relate … summarizes`), and tombstones the sources (dropped from recall,
reclaimed by compaction, recoverable from the log until then). Off by default
(`summary_mode=none`); run explicitly with `aegisdb-summarize` from cron / a
timer / the compose sidecar — never on the per-turn hot path.
"""
from __future__ import annotations

import argparse
import json
import sys
import time

from .client import AegisClient
from .config import load_config
from .embeddings import make_provider
from .summary import make_summary_provider
from .tools import MemoryTools


def _importance(rec: dict) -> float:
    v = rec.get("importance")
    return 0.5 if v is None else float(v)


def _clusters(memories: list[dict], config) -> list[tuple[str, list[dict]]]:
    """Group candidate episodic memories into disjoint clusters by shared tag.

    Only episodic memories at or below the importance ceiling are eligible; a
    record is placed in the first (alphabetical) tag's cluster and not reused, so
    clusters don't overlap. Clusters smaller than the minimum are dropped, and
    the total is capped per run."""
    eligible = [
        m for m in memories
        if m.get("kind") == "episodic"
        and _importance(m) <= config.summary_max_importance
        and "summary" not in (m.get("tags") or [])
    ]
    by_tag: dict[str, list[dict]] = {}
    for m in eligible:
        for tag in (m.get("tags") or []):
            by_tag.setdefault(tag, []).append(m)

    clusters: list[tuple[str, list[dict]]] = []
    placed: set = set()
    for tag in sorted(by_tag):
        recs = [m for m in by_tag[tag] if m["id"] not in placed]
        if len(recs) < config.summary_min_cluster:
            continue
        recs = recs[: config.summary_max_cluster]
        for m in recs:
            placed.add(m["id"])
        clusters.append((tag, recs))
        if len(clusters) >= config.summary_max_clusters_per_run:
            break
    return clusters


def run_summarize(config, client, summary_provider, embed_provider, *,
                  dry_run: bool = False, now_ms: int | None = None) -> dict:
    """Run one distillation pass. `summary_provider` distils clusters;
    `embed_provider` is the embedding provider MemoryTools uses for its
    reads/writes. Returns a report dict (never raises for the normal
    'nothing to do' / provider-off cases)."""
    if not summary_provider.available():
        return {"ok": False, "clusters": 0, "summarized": 0, "archived": 0,
                "error": "summary provider unavailable — set AEGIS_SUMMARY_MODE "
                         "(and ensure the backend is reachable)"}

    tools = MemoryTools(config, client, embed_provider)
    provider = summary_provider
    now = now_ms if now_ms is not None else int(time.time() * 1000)
    cutoff = max(0, now - int(config.summary_min_age_ms))

    res = tools.search(start_time=0, end_time=cutoff,
                       top_k=config.summary_scan_top_k)
    if not res.get("ok"):
        return {"ok": False, "clusters": 0, "summarized": 0, "archived": 0,
                "error": res.get("error", "candidate search failed")}

    clusters = _clusters(res.get("memories", []), config)
    report = {"ok": True, "dry_run": dry_run, "clusters": len(clusters),
              "summarized": 0, "archived": 0, "summaries": []}

    for tag, recs in clusters:
        texts = [m["text"] for m in recs if m.get("text")]
        out = provider.summarize(texts)
        if not out:
            continue
        summary, confidence = out
        if confidence < config.summary_min_confidence:
            continue
        source_ids = [m["id"] for m in recs]
        tags = sorted({t for m in recs for t in (m.get("tags") or [])} | {"summary"})
        report["summaries"].append(
            {"tag": tag, "sources": source_ids, "confidence": confidence,
             "summary": summary})
        if dry_run:
            continue

        sv = tools.save(summary, tags=tags,
                        importance=max(_importance(m) for m in recs),
                        semantic=True, confidence=confidence)
        sid = sv.get("id")
        if not sid:
            continue  # write failed (e.g. memory cap / unavailable); skip cluster
        report["summarized"] += 1
        # Provenance first (the source must exist to relate), then archive it.
        for src in source_ids:
            tools.relate(sid, src, "summarizes")
            if tools.delete(src).get("ok"):
                report["archived"] += 1
    return report


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="aegisdb-summarize",
        description="Distil clusters of aging memories into semantic facts. "
                    "Operator-scheduled (cron/timer); off unless AEGIS_SUMMARY_MODE "
                    "is set.")
    ap.add_argument("--dry-run", action="store_true",
                    help="select + summarize but write nothing; print the plan")
    args = ap.parse_args(argv)

    config = load_config()
    summary_provider = make_summary_provider(config)
    embed_provider = make_provider(config)
    client = AegisClient.from_config(config)
    report = run_summarize(config, client, summary_provider, embed_provider,
                           dry_run=args.dry_run)
    json.dump(report, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0 if report.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())