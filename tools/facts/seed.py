#!/usr/bin/env python3
"""Load a typed-fact corpus into a running AegisDB.

    ./build/aegisdb --data-dir ./data --port 9470 \
        --predicate-registry predicates.example.json --inference &
    python3 tools/facts/seed.py --facts tools/facts/aegisdb.json

Two passes, because a fact names its subject and object by **record id** and
those ids do not exist until the entity records are written. The corpus refers
to them by label; this is where labels become ids.

Entities are written tagged `entity` — the convention `grounding.py` resolves
against, so a later model-extracted triple about "the storage layer" lands on
the record this seeded rather than minting a second one.

Idempotent: an entity whose exact prose already exists is reused, and a fact
the corpus already asserts is skipped. Running it twice does not double the
store, which matters because the interesting thing to do with it is run it,
look, adjust the corpus, and run it again.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "clients", "python"))

from aegisdb import AegisClient, AegisError  # noqa: E402

ENTITY_TAG = "entity"
FACT_TAG = "fact"


def load_registry(path: str) -> dict:
    """The vocabulary, as the server reads it.

    The same file the server was started with, deliberately — a second copy
    would drift, and the corpus would then assert predicates the server
    refuses, which looks like a bad corpus rather than a misconfiguration.
    """
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def find_entity(db: AegisClient, prose: str) -> int | None:
    """An existing entity record with exactly this prose, or None."""
    try:
        hits = db.search(query=prose, tags=[ENTITY_TAG], top_k=10)
    except AegisError:
        return None
    for m in hits.get("records") or []:
        if (m.get("data") or "").strip() == prose.strip():
            return m["id"]
    return None


def seed(db: AegisClient, corpus: dict, registry: dict, *,
         dry_run: bool = False) -> dict:
    report = {"entities_created": 0, "entities_reused": 0,
              "facts_written": 0, "facts_present": 0, "refused": []}

    ids: dict[str, int] = {}
    for label, prose in corpus["entities"].items():
        found = find_entity(db, prose)
        if found is not None:
            ids[label] = found
            report["entities_reused"] += 1
            continue
        if dry_run:
            # A placeholder id: nothing is written, so the facts below cannot
            # be probed for presence either — they are counted as new, which
            # is what they would be.
            ids[label] = -1
            report["entities_created"] += 1
            continue
        ids[label] = db.insert(prose, type="semantic",
                               tags=[ENTITY_TAG])["record"]["id"]
        report["entities_created"] += 1

    for row in corpus["facts"]:
        subject, predicate, obj, prose = row
        spec = registry.get(predicate)
        if spec is None:
            # Refused here rather than sent: the server would refuse it too,
            # and saying so once with the predicate named is more use than a
            # wall of INVALID_REQUEST.
            report["refused"].append((predicate, "not in the registry"))
            continue
        if spec.get("object") == "id":
            if obj not in ids:
                report["refused"].append((predicate, f"unknown entity {obj!r}"))
                continue
            fact = {"s": ids[subject], "p": predicate, "o": {"id": ids[obj]}}
        else:
            fact = {"s": ids[subject], "p": predicate, "o": obj}

        # Already asserted? An exact pattern is an index probe, so this is
        # cheap enough to ask per fact and is what makes re-running safe.
        #
        # Asked on a dry run too, even though it costs a round trip per fact:
        # a dry run reporting "37 to write" against a store that already holds
        # all 37 is worse than no dry run at all. But only when every id in the
        # fact is real — on a dry run against a store missing an entity, the id
        # is the -1 placeholder above and probing for it is a malformed
        # request, not a miss.
        probeable = ids[subject] > 0 and (
            spec.get("object") != "id" or ids[obj] > 0)
        if probeable and (db.search(pattern=fact, top_k=1).get("records") or []):
            report["facts_present"] += 1
            continue
        if dry_run:
            report["facts_written"] += 1
            continue
        try:
            db.insert(prose, type="semantic", tags=[FACT_TAG], fact=fact)
            report["facts_written"] += 1
        except AegisError as exc:
            report["refused"].append((predicate, str(exc)))
    return report


def main(argv=None) -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--facts", default=os.path.join(here, "aegisdb.json"),
                    help="corpus to load")
    ap.add_argument("--registry",
                    default=os.path.join(root, "predicates.example.json"),
                    help="the SAME file the server was started with")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9470)
    ap.add_argument("--token", default="")
    ap.add_argument("--namespace", default=None,
                    help="AegisDB namespace to write into (default: none)")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would be written, and write nothing")
    args = ap.parse_args(argv)

    with open(args.facts, encoding="utf-8") as fh:
        corpus = json.load(fh)
    registry = load_registry(args.registry)

    with AegisClient(host=args.host, port=args.port, token=args.token,
                     agent_id=args.namespace, read_timeout=30) as db:
        if not db.available():
            print(f"no aegisdb at {args.host}:{args.port}", file=sys.stderr)
            return 2
        report = seed(db, corpus, registry, dry_run=args.dry_run)

    print(f"corpus         {corpus.get('name')}"
          f"{'  (dry run — nothing written)' if args.dry_run else ''}")
    print(f"entities       {report['entities_created']} created, "
          f"{report['entities_reused']} reused")
    print(f"facts          {report['facts_written']} written, "
          f"{report['facts_present']} already present")
    if report["refused"]:
        print(f"refused        {len(report['refused'])}:")
        for pred, why in report["refused"][:10]:
            print(f"  {pred}: {why}")
    return 1 if report["refused"] else 0


if __name__ == "__main__":
    sys.exit(main())
