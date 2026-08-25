"""`aegisdb-seed` — load a project's typed-fact corpora into its AegisDB.

    aegisdb-seed --dry-run     # report what would be written
    aegisdb-seed

Everything is discovered from `.aegisdb/` rather than passed: the registry from
`.aegisdb/predicates.json`, the corpora from every `*.json` under
`.aegisdb/facts/`, and host/port/namespace from `.aegisdb/config.json` — the
same file the MCP server and the hooks read, so a corpus lands in the namespace
the agent recalls from instead of a namespace someone typed twice.

Two passes, because a fact names its subject and object by **record id** and
those ids do not exist until the entity records are written. The corpus refers
to them by label; this is where labels become ids.

Idempotent: an entity whose exact prose already exists is reused, and a fact the
corpus already asserts is skipped. Running it twice does not double the store,
which matters because the interesting thing to do with a corpus is run it, look
at the result, adjust it, and run it again.

(`tools/facts/seed.py` in the AegisDB repo does the same job for the repo's own
corpora against the `aegisdb` SDK client. This one exists because a consumer
project has no checkout to run that from.)
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys

from . import config as config_mod
from .client import AegisClient, AegisUnavailable
from .config import load_config

ENTITY_TAG = "entity"
FACT_TAG = "fact"

DEFAULT_PREDICATES = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "default_predicates.json")


def registry_path(env=None, cwd: str | None = None) -> str:
    """`.aegisdb/predicates.json` — the project's vocabulary."""
    return os.path.join(config_mod.project_dir(env, cwd), "predicates.json")


def facts_dir(env=None, cwd: str | None = None) -> str:
    """`.aegisdb/facts/` — where a project's corpora live."""
    return os.path.join(config_mod.project_dir(env, cwd), "facts")


def discover_corpora(env=None, cwd: str | None = None) -> list[str]:
    """Every `*.json` directly under `.aegisdb/facts/`, in name order.

    Sorted so a run is reproducible: entity ids are assigned in the order the
    corpora are read, and a directory listing is not ordered.
    """
    return sorted(glob.glob(os.path.join(facts_dir(env, cwd), "*.json")))


def load_registry(path: str) -> dict:
    """The vocabulary, as the server reads it.

    Falls back to the bundled starter vocabulary when the project has none, so
    `aegisdb-seed` works before anyone has authored one — but note the *server*
    still has to be started with `--predicate-registry`, and if it was started
    with a different file than this one, it is the server's copy that decides
    what is accepted. Same file, both places.
    """
    if not os.path.isfile(path):
        path = DEFAULT_PREDICATES
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def _entity_id(db: AegisClient, prose: str, namespace: str) -> int | None:
    """An existing entity record with exactly this prose, or None.

    Matched on the payload rather than on a stored label, because the prose *is*
    the identity — that is what lets two corpora written months apart land on
    one record instead of minting a second.
    """
    resp = db.request({"operation": "search", "query": prose,
                       "tags": [ENTITY_TAG], "match": "any", "top_k": 10,
                       "agent_id": namespace or None,
                       "include_embeddings": False})
    for rec in resp.get("records") or []:
        if (rec.get("data") or "").strip() == prose.strip():
            return rec["id"]
    return None


def _insert(db: AegisClient, data: str, tags: list, namespace: str,
            fact: dict | None = None) -> dict:
    payload = {"operation": "insert", "type": "semantic", "data": data,
               "tags": tags, "include_embeddings": False}
    if namespace:
        payload["agent_id"] = namespace
    if fact:
        payload["fact"] = fact
    resp = db.request(payload)
    if not resp.get("ok"):
        raise RuntimeError((resp.get("error") or {}).get("message") or "insert failed")
    return resp["record"]


def seed_corpus(db: AegisClient, corpus: dict, registry: dict, *,
                namespace: str = "", dry_run: bool = False) -> dict:
    """Write one corpus. Returns a per-corpus report."""
    report = {"name": corpus.get("name") or "?", "entities_created": 0,
              "entities_reused": 0, "facts_written": 0, "facts_present": 0,
              "refused": []}

    ids: dict[str, int] = {}
    for label, prose in (corpus.get("entities") or {}).items():
        found = _entity_id(db, prose, namespace)
        if found is not None:
            ids[label] = found
            report["entities_reused"] += 1
        elif dry_run:
            # A placeholder: nothing is written, so the facts below cannot be
            # probed for presence either — they count as new, which is what
            # they would be.
            ids[label] = -1
            report["entities_created"] += 1
        else:
            ids[label] = _insert(db, prose, [ENTITY_TAG], namespace)["id"]
            report["entities_created"] += 1

    for row in corpus.get("facts") or []:
        try:
            subject, predicate, obj, prose = row
        except ValueError:
            report["refused"].append((str(row)[:40], "not a [s, p, o, prose] row"))
            continue
        spec = registry.get(predicate)
        if spec is None:
            # Refused here rather than sent: the server would refuse it too, and
            # saying so once with the predicate named is more use than a wall of
            # INVALID_REQUEST.
            report["refused"].append((predicate, "not in the registry"))
            continue
        if subject not in ids:
            report["refused"].append((predicate, f"unknown subject {subject!r}"))
            continue
        if spec.get("object") == "id":
            if obj not in ids:
                report["refused"].append((predicate, f"unknown entity {obj!r}"))
                continue
            fact = {"s": ids[subject], "p": predicate, "o": {"id": ids[obj]}}
        else:
            fact = {"s": ids[subject], "p": predicate, "o": obj}

        probeable = ids[subject] > 0 and (
            spec.get("object") != "id" or ids[obj] > 0)
        if probeable:
            hit = db.request({"operation": "search", "pattern": fact, "top_k": 1,
                              "agent_id": namespace or None,
                              "include_embeddings": False})
            if hit.get("records"):
                report["facts_present"] += 1
                continue
        if dry_run:
            report["facts_written"] += 1
            continue
        try:
            _insert(db, prose, [FACT_TAG], namespace, fact=fact)
            report["facts_written"] += 1
        except RuntimeError as exc:
            report["refused"].append((predicate, str(exc)))
    return report


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="aegisdb-seed",
        description="Load .aegisdb/facts/*.json into this project's AegisDB, "
                    "using .aegisdb/predicates.json as the vocabulary.")
    ap.add_argument("--dir", default=".", help="project directory (default: cwd)")
    ap.add_argument("--facts", action="append", default=None,
                    help="corpus file (repeatable); default: every "
                         "*.json in .aegisdb/facts/")
    ap.add_argument("--registry", default=None,
                    help="the SAME file the server was started with; default: "
                         ".aegisdb/predicates.json, else the bundled starter")
    ap.add_argument("--namespace", default=None,
                    help="override the namespace from .aegisdb/config.json")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would be written, and write nothing")
    args = ap.parse_args(argv)

    proj = os.path.abspath(args.dir)
    cfg = load_config(cwd=proj)
    namespace = args.namespace if args.namespace is not None else cfg.namespace
    corpora = args.facts or discover_corpora(cwd=proj)
    if not corpora:
        print(f"aegisdb-seed: no corpora in {facts_dir(cwd=proj)} — add a "
              f"*.json corpus there, or pass --facts", file=sys.stderr)
        return 1

    reg_path = args.registry or registry_path(cwd=proj)
    registry = load_registry(reg_path)
    using = reg_path if os.path.isfile(reg_path) else DEFAULT_PREDICATES

    db = AegisClient.from_config(cfg)
    try:
        if not db.available():
            print(f"aegisdb-seed: no aegisdb at {cfg.aegis_host}:{cfg.aegis_port}",
                  file=sys.stderr)
            return 2
    except AegisUnavailable as exc:
        print(f"aegisdb-seed: {exc}", file=sys.stderr)
        return 2

    reports = []
    for path in corpora:
        with open(path, encoding="utf-8") as fh:
            corpus = json.load(fh)
        reports.append(seed_corpus(db, corpus, registry, namespace=namespace,
                                   dry_run=args.dry_run))

    suffix = "  (dry run — nothing written)" if args.dry_run else ""
    print(f"registry       {os.path.relpath(using, proj) if using.startswith(proj) else using}"
          f" ({len(registry)} predicates)")
    print(f"namespace      {namespace or '(from the auth token)'}{suffix}")
    refused = []
    for r in reports:
        print(f"  {r['name']:<24} entities {r['entities_created']} created, "
              f"{r['entities_reused']} reused; facts {r['facts_written']} written, "
              f"{r['facts_present']} already present")
        refused.extend(r["refused"])
    for pred, why in refused:
        print(f"  refused: {pred} — {why}", file=sys.stderr)
    return 1 if refused else 0


if __name__ == "__main__":
    raise SystemExit(main())
