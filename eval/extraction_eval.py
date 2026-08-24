#!/usr/bin/env python3
"""Extraction eval: does prose become the triples a careful reader would write?

ROADMAP 5.4's "done when" is a measurement, not a feature: *triples that snap to
the registry — measured as an in-vocabulary rate, not asserted*. This is that
scoreboard, and it reports three numbers rather than one:

- **In-vocabulary rate** — accepted over testable. The ratio the horizon is
  judged on, because a registry that rejects most of what the model proposes and
  one that fits the corpus look identical from the accepted count alone.
- **Conflation** — two distinct things that ended up sharing one entity id.
  Facts then describe the wrong entity, 5.3 derives more of them, and nothing in
  the system can notice. Unrecoverable, so it gates at zero.
- **Fragmentation** — one thing split across several ids. Only inferences that
  would have crossed the split are lost, nothing false is asserted, and
  `consolidate` can merge the records afterwards.

The two grounding errors are counted **apart and never summed**. A single
"grounding accuracy" would average an unrecoverable error against a recoverable
one and hide exactly the asymmetry §4 of `neuro-symbolic-design.md` is built
around.

    make eval-extraction
    python3 eval/extraction_eval.py ./build/aegisdb --extractor anthropic
    python3 eval/extraction_eval.py ./build/aegisdb --json

**Read the fake number with its caveat.** The deterministic `fake` backend
parses a line format, not English, so each transcript carries a `cues` block it
can read. Under `--extractor fake` the in-vocabulary rate is therefore a
property of the dataset — a regression gate on the pipeline, and not a claim
about any model's reading comprehension. Point `--extractor` at a real provider
for the number that means something. The harness says so on every fake run
rather than trusting anyone to remember.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from recall_eval import Server  # noqa: E402  (reuse the spawn + wire client)

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO, "integrations", "claude-code"))
from aegis_mcp.client import AegisClient  # noqa: E402
from aegis_mcp.config import load_config  # noqa: E402
from aegis_mcp.embeddings import FakeProvider, LocalProvider  # noqa: E402
from aegis_mcp.extract import (load_vocabulary,  # noqa: E402
                               make_extraction_provider)
from aegis_mcp.grounding import normalize  # noqa: E402
from aegis_mcp.tools import MemoryTools  # noqa: E402
from aegis_mcp.triples import store_triples  # noqa: E402


def canon_map(entities: dict) -> dict:
    """Every surface form -> the gold entity it denotes.

    Normalized with grounding's own `normalize`, not a second spelling of it:
    a scorer that canonicalized differently from the thing under test would
    report differences that are the scorer's, not the extractor's.
    """
    out = {}
    for gold, aliases in entities.items():
        for alias in [gold] + list(aliases):
            out[normalize(alias)] = gold
    return out


def transcript_input(t: dict, extractor_name: str) -> str:
    """What the extractor is shown.

    A real backend reads the prose, which is the thing under test. The `fake`
    one parses `S : p : O` lines, so it is shown the cue block appended to the
    prose — feeding it the prose alone would measure nothing at all, and
    feeding a real model the cues would measure the dataset instead of the
    model.
    """
    text = t.get("text", "")
    if extractor_name == "fake":
        return text + "\n" + "\n".join(t.get("cues", []))
    return text


def read_back(tools, predicates: dict) -> list:
    """Every triple the store actually holds, as (subject, predicate, object).

    Read through `pattern` rather than trusted from the writer's own return
    value, on the same principle the rest of 5.4 follows: the server has the
    last word on what was stored. Subjects and id-valued objects come back as
    ids and are resolved to the entity record's prose, which is the mention
    grounding chose to keep.
    """
    texts = {}

    def text_of(rec_id):
        if rec_id not in texts:
            got = tools.get(rec_id)
            mem = got.get("memory") or {}
            texts[rec_id] = (mem.get("text") or "") if got.get("ok") else ""
        return texts[rec_id]

    out = []
    for pred, spec in predicates.items():
        res = tools.search(pattern={"p": pred}, top_k=500)
        for m in res.get("memories", []):
            fact = m.get("fact") or {}
            if fact.get("p") != pred:
                continue
            subject = text_of(fact.get("s"))
            obj = fact.get("o")
            if isinstance(obj, dict) and "id" in obj:
                obj = text_of(obj["id"])
            out.append((subject, pred, str(obj)))
    return out


def canonical(triple, canon: dict, predicates: dict):
    """A triple with its entity positions collapsed onto gold entities.

    Deliberately applied to *both* sides of the comparison. Fragmentation is
    measured on its own, and letting it also depress the triple score would
    charge one error twice — and hide which of the two actually moved.
    """
    s, p, o = triple
    spec = predicates.get(p) or {}
    cs = canon.get(normalize(s), normalize(s))
    co = canon.get(normalize(o), normalize(o)) if spec.get("object") == "id" \
        else normalize(o)
    return (cs, p, co)


def grounding_errors(mention_ids: dict, canon: dict):
    """Split the grounding record into conflation and fragmentation.

    `mention_ids` is every mention grounding placed, across the whole run.
    Mentions the dataset does not label are reported separately rather than
    scored: an unlabelled mention says the dataset is incomplete, which is not
    the same finding as grounding being wrong, and folding the two together
    would let a thin dataset look like a clean run.
    """
    by_gold, by_id, unlabelled = {}, {}, set()
    for mention, rec_id in mention_ids.items():
        gold = canon.get(normalize(mention))
        if gold is None:
            unlabelled.add(mention)
            continue
        by_gold.setdefault(gold, set()).add(rec_id)
        by_id.setdefault(rec_id, set()).add(gold)
    fragmented = {g: ids for g, ids in by_gold.items() if len(ids) > 1}
    conflated = {i: gs for i, gs in by_id.items() if len(gs) > 1}
    return conflated, fragmented, sorted(unlabelled)


def run(args) -> int:
    with open(args.dataset, encoding="utf-8") as fh:
        ds = json.load(fh)
    predicates = ds.get("predicates") or {}
    canon = canon_map(ds.get("entities") or {})

    embed = FakeProvider(ds.get("embedding_dim", 64)) if args.embedder == "fake" \
        else LocalProvider()
    dim = embed.dimension()

    fd, registry = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w") as fh:
        json.dump(predicates, fh)

    totals = {k: 0 for k in ("proposed", "rejected", "malformed", "stored",
                             "duplicate", "ungrounded", "failed",
                             "entities_resolved", "entities_minted")}
    mention_ids, per_transcript = {}, []
    try:
        with Server(args.binary, args.port, dim,
                    extra_args=["--predicate-registry", registry]) as srv:
            cfg = load_config(env={
                "AEGIS_HOST": "127.0.0.1", "AEGIS_PORT": str(srv.port),
                "AEGIS_NAMESPACE": "extraction-eval",
                "AEGIS_EMBEDDING_DIMENSIONS": str(dim),
                "AEGIS_EMBEDDING_MODE": "none",
            }, overrides={"extract_triples": True,
                          "extract_registry": registry,
                          "extract_mode": args.extractor,
                          "extract_model": args.model,
                          "extract_max_triples": args.max_triples})
            tools = MemoryTools(cfg, AegisClient.from_config(cfg), embed)
            vocab = load_vocabulary(registry)
            # The same factory capture uses, so the eval cannot accidentally
            # measure a backend nobody runs. It falls back to the `none`
            # provider when the selected one is unavailable — which for an eval
            # would silently report a perfect zero, so that case is refused
            # rather than scored.
            extractor = make_extraction_provider(cfg)
            if not extractor.available():
                print(f"extractor {args.extractor!r} is not available here "
                      f"(missing API key, or the CLI is not on PATH)",
                      file=sys.stderr)
                return 2

            for t in ds["transcripts"]:
                res = store_triples(tools, transcript_input(t, args.extractor),
                                    vocab, cfg, extractor)
                for k in totals:
                    totals[k] += getattr(res, k)
                for mention, rec_id in res.entity_ids.items():
                    # First placement wins. A later transcript resolving to the
                    # same id is the system working; recording it twice would
                    # make a stable mention look like it moved.
                    mention_ids.setdefault(mention, rec_id)
                per_transcript.append({
                    "label": t.get("label"), "proposed": res.proposed,
                    "rejected": res.rejected, "stored": res.stored,
                    "in_vocabulary_rate": res.in_vocabulary_rate,
                })
            held = read_back(tools, predicates)
    finally:
        os.unlink(registry)

    testable = totals["proposed"] - totals["malformed"]
    in_vocab = ((testable - totals["rejected"]) / testable) if testable else 0.0

    gold = {canonical((g["s"], g["p"], g["o"]), canon, predicates)
            for t in ds["transcripts"] for g in t.get("gold", [])}
    got = {canonical(x, canon, predicates) for x in held}
    hit = gold & got
    recall = len(hit) / len(gold) if gold else 0.0
    precision = len(hit) / len(got) if got else 0.0

    conflated, fragmented, unlabelled = grounding_errors(mention_ids, canon)
    unstatable = sum(len(t.get("unstatable") or []) for t in ds["transcripts"])

    report = {
        "dataset": ds.get("name"), "extractor": args.extractor,
        "embedder": args.embedder, "counts": totals,
        "in_vocabulary_rate": in_vocab,
        "unstatable": unstatable,
        "gold": {"recall": recall, "precision": precision,
                 "expected": len(gold), "held": len(got),
                 "missing": sorted(gold - got), "extra": sorted(got - gold)},
        "grounding": {
            "conflated": {str(k): sorted(v) for k, v in conflated.items()},
            "fragmented": {k: sorted(v) for k, v in fragmented.items()},
            "unlabelled_mentions": unlabelled,
        },
        "per_transcript": per_transcript,
    }
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        emit(report, ds)
    return gate(args, report)


def emit(r: dict, ds: dict) -> None:
    c = r["counts"]
    print(f"\nextraction eval · dataset={r['dataset']} · "
          f"extractor={r['extractor']} · embedder={r['embedder']}")
    print(f"  transcripts        {len(ds['transcripts'])}")
    print(f"  proposed           {c['proposed']}")
    print(f"  malformed          {c['malformed']}   (never reached the registry)")
    print(f"  rejected           {c['rejected']}   (out of vocabulary)")
    print(f"  IN-VOCABULARY      {r['in_vocabulary_rate']:.1%}")
    print(f"\n  stored             {c['stored']}   "
          f"duplicate {c['duplicate']} · ungrounded {c['ungrounded']} · "
          f"failed {c['failed']}")
    print(f"  entities           minted {c['entities_minted']} · "
          f"resolved {c['entities_resolved']}")

    g = r["grounding"]
    print("\n  grounding — counted apart, never summed")
    print(f"    conflation       {len(g['conflated'])} id(s)   "
          f"two things sharing one id · unrecoverable")
    for rec_id, golds in g["conflated"].items():
        print(f"                     id {rec_id} ← {', '.join(golds)}")
    frag_ids = sum(len(v) for v in g["fragmented"].values())
    print(f"    fragmentation    {len(g['fragmented'])} entit(y/ies) → "
          f"{frag_ids} ids   one thing split · consolidate can merge")
    for gold, ids in g["fragmented"].items():
        print(f"                     {gold} → {ids}")
    if g["unlabelled_mentions"]:
        print(f"    unlabelled       {len(g['unlabelled_mentions'])} mention(s) "
              f"the dataset does not name: "
              f"{', '.join(g['unlabelled_mentions'][:5])}")

    gold = r["gold"]
    print(f"\n  triples vs gold    recall {gold['recall']:.1%} "
          f"({gold['expected']} expected, {gold['held']} held) · "
          f"{len(gold['extra'])} beyond gold")
    for m in gold["missing"]:
        print(f"    missing          {m[0]} {m[1]} {m[2]}")
    for e in gold["extra"]:
        # "Beyond gold", not "wrong": the dataset's gold list is a floor — the
        # triples a careful reader would certainly write — so a fact the store
        # holds and gold does not name may be perfectly true. Labelling these
        # as errors would make an extractor that reads *more* of the transcript
        # score worse, which is backwards.
        print(f"    beyond gold      {e[0]} {e[1]} {e[2]}")
    print(f"\n  unstatable         {r['unstatable']} triple(s) a careful reader "
          f"would write and the registry cannot express")

    if r["extractor"] == "fake":
        print("\n  NOTE: `fake` parses the dataset's cue block, not English. The "
              "in-vocabulary\n        rate above is a property of this dataset — a "
              "regression gate on the\n        pipeline, not a model score. Use "
              "--extractor claude-code|anthropic|openai\n        for the number "
              "5.4 is actually judged on.")


def gate(args, r: dict) -> int:
    failed = []
    if args.gate_in_vocabulary is not None and \
            r["in_vocabulary_rate"] < args.gate_in_vocabulary:
        failed.append(f"in-vocabulary {r['in_vocabulary_rate']:.1%} < "
                      f"{args.gate_in_vocabulary:.1%}")
    # Conflation gates at zero by default and the flag only ever raises the
    # ceiling. It is the one error the rest of the system cannot detect, so a
    # run that commits it has not "scored slightly worse" — it has written
    # facts about the wrong entity.
    if len(r["grounding"]["conflated"]) > args.max_conflation:
        failed.append(f"conflation {len(r['grounding']['conflated'])} > "
                      f"{args.max_conflation}")
    # Fragmentation has a ceiling rather than a floor of zero: the design
    # *chooses* it over conflation, so some is the system working as argued.
    # What the gate is for is the other failure — a threshold change that
    # starts minting for every mention, which shows up here long before it
    # shows up as a graph nobody can reason over.
    if args.max_fragmentation is not None and \
            len(r["grounding"]["fragmented"]) > args.max_fragmentation:
        failed.append(f"fragmentation {len(r['grounding']['fragmented'])} > "
                      f"{args.max_fragmentation}")
    if args.gate_gold_recall is not None and \
            r["gold"]["recall"] < args.gate_gold_recall:
        failed.append(f"gold recall {r['gold']['recall']:.1%} < "
                      f"{args.gate_gold_recall:.1%}")
    if failed:
        print("\nGATE FAILED: " + "; ".join(failed), file=sys.stderr)
        return 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("binary", nargs="?", default="./build/aegisdb")
    ap.add_argument("--dataset", default="eval/datasets/extraction.json")
    ap.add_argument("--extractor", default="fake",
                    choices=["fake", "claude-code", "anthropic", "openai"])
    ap.add_argument("--model", default="",
                    help="model id for a real extractor; the provider default "
                         "when empty")
    ap.add_argument("--embedder", default="fake", choices=["fake", "local"],
                    help="grounding's similarity pass runs on this")
    ap.add_argument("--port", type=int, default=9973)
    ap.add_argument("--max-triples", type=int, default=16,
                    help="cap on candidates proposed per transcript")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--gate-in-vocabulary", type=float, default=None,
                    help="fail below this in-vocabulary rate")
    ap.add_argument("--max-conflation", type=int, default=0,
                    help="entity ids denoting more than one thing (0 = none)")
    ap.add_argument("--max-fragmentation", type=int, default=None,
                    help="gold entities split across more than one id")
    ap.add_argument("--gate-gold-recall", type=float, default=None,
                    help="fail below this share of the gold triples")
    return run(ap.parse_args())


if __name__ == "__main__":
    sys.exit(main())
