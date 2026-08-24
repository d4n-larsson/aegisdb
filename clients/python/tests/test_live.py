"""Every method, against a real aegisdb.

The point is coverage of the *surface*, not of the server's behaviour: the
server ignores request fields it does not recognise, so a misspelled field name
in this client would otherwise be invisible — the call would succeed and
quietly do the wrong thing. So each method is called and its effect asserted,
which is the only way a typo here becomes a failure.

Skipped when the binary is not built.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from aegisdb import (AegisClient, Forbidden, Immutable, NotFound,  # noqa: E402
                     NotReady, Unauthorized)

_HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(_HERE)))
BINARY = os.path.join(REPO, "build", "aegisdb")

REGISTRY = {
    "part_of": {"object": "id", "transitive": True},
    "defaults_to": {"object": "string", "cardinality": "one"},
}


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, *, dim=8, extra=None, token_lines=None):
        self.port = _free_port()
        self.datadir = tempfile.mkdtemp(prefix="aegis_sdk_")
        self.dim = dim
        self.extra = list(extra or [])
        self.token_lines = token_lines
        self.proc = None

    def __enter__(self):
        args = [BINARY, "--data-dir", self.datadir, "--port", str(self.port),
                "--embedding-dim", str(self.dim)] + self.extra
        if self.token_lines:
            tf = os.path.join(self.datadir, "tokens")
            with open(tf, "w") as fh:
                fh.write("\n".join(self.token_lines) + "\n")
            args += ["--auth-token-file", tf]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        for _ in range(60):
            try:
                with socket.create_connection(("127.0.0.1", self.port),
                                              timeout=0.2):
                    return self
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("aegisdb did not start")

    def __exit__(self, *exc):
        if self.proc:
            self.proc.kill()
            self.proc.wait()
        return False


def _registry():
    fd, path = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w") as fh:
        json.dump(REGISTRY, fh)
    return path


@unittest.skipUnless(os.path.exists(BINARY), "aegisdb binary not built")
class TestLiveSurface(unittest.TestCase):
    def test_health_and_stats(self):
        with Server() as srv, AegisClient(port=srv.port) as db:
            self.assertTrue(db.ping()["ok"])
            self.assertTrue(db.available())
            st = db.stats()
            self.assertIn("indexes", st)

    def test_insert_get_update_delete_history(self):
        with Server() as srv, AegisClient(port=srv.port) as db:
            rec = db.insert("prefers dark mode", type="semantic",
                            tags=["user", "pref"], importance=0.7,
                            confidence=0.9)["record"]
            rid = rec["id"]
            self.assertEqual(rec["type"], "semantic")
            self.assertEqual(sorted(rec["tags"]), ["pref", "user"])
            # importance/confidence really landed — the fields the ranking uses.
            self.assertAlmostEqual(rec["importance"], 0.7, places=5)
            self.assertAlmostEqual(rec["confidence"], 0.9, places=5)

            got = db.get(rid)["record"]
            self.assertEqual(got["data"], "prefers dark mode")

            # Validity intervals are millisecond-grained, so an update in the
            # same millisecond as the insert gives the first version a
            # zero-width interval — there is then no instant at which it was
            # live, and `as_of` rightly returns the second. Sleeping past the
            # boundary is what makes the point-in-time read testable at all;
            # without it this passes or fails on how fast the machine is, which
            # is how it passed locally and failed in CI.
            time.sleep(0.01)
            db.update(rid, data="prefers light mode", importance=0.2)
            self.assertEqual(db.get(rid)["record"]["data"], "prefers light mode")

            hist = db.history(rid)["versions"]
            self.assertEqual(len(hist), 2, f"{hist}")
            first, second = hist
            # Assert the precondition rather than assume it: a zero-width
            # interval would make the next assertion vacuous instead of failing.
            self.assertLess(first["valid_from"], first["valid_to"],
                            f"the first version was never live: {hist}")
            self.assertEqual(first["valid_to"], second["valid_from"],
                             "the intervals abut")
            self.assertEqual(second["valid_to"], 0, "the live version is open")

            # as_of anywhere inside the first interval reaches the old version.
            for at in (first["valid_from"], first["valid_to"] - 1):
                self.assertEqual(db.get(rid, as_of=at)["record"]["data"],
                                 "prefers dark mode", f"as_of={at}")
            # ...and at the boundary itself, the new one — the interval is
            # half-open, so valid_to belongs to the version that follows.
            self.assertEqual(
                db.get(rid, as_of=first["valid_to"])["record"]["data"],
                "prefers light mode")

            db.delete(rid)
            with self.assertRaises(NotFound):
                db.get(rid)

    def test_episodic_is_immutable(self):
        with Server() as srv, AegisClient(port=srv.port) as db:
            rid = db.insert("something happened")["record"]["id"]
            with self.assertRaises(Immutable):
                db.update(rid, data="something else happened")

    def test_insert_many(self):
        with Server() as srv, AegisClient(port=srv.port) as db:
            res = db.insert_many([
                {"type": "semantic", "data": "one", "tags": ["b"]},
                {"type": "semantic", "data": "two", "tags": ["b"]},
            ])
            self.assertTrue(res["ok"], res)
            self.assertEqual(db.count(tags=["b"])["count"], 2)

    def test_search_paths(self):
        with Server() as srv, AegisClient(port=srv.port) as db:
            db.insert("the --tenant-max-records flag caps a namespace",
                      type="semantic", tags=["flag"])
            db.insert("unrelated note about coffee", type="semantic",
                      tags=["other"])

            # keyword: the exact identifier, which is the whole point of BM25
            hits = db.search(query="--tenant-max-records", top_k=5)["records"]
            self.assertEqual(len(hits), 1, hits)
            self.assertIn("tenant-max-records", hits[0]["data"])

            # tags, time, explain, offset, order
            self.assertEqual(len(db.search(tags=["flag"], match="all")["records"]), 1)
            self.assertEqual(len(db.search(start_time=0,
                                           end_time=9 * 10 ** 12,
                                           top_k=10)["records"]), 2)
            ex = db.search(query="coffee", explain=True, top_k=1)["records"][0]
            self.assertIn("explain", ex)
            self.assertEqual(db.search(tags=["flag"], offset=1)["records"], [])
            self.assertEqual(len(db.search(tags=["other"], order="oldest",
                                           top_k=1)["records"]), 1)

            # embedding search, and include_embeddings round-tripping
            vec = [0.0] * srv.dim
            vec[0] = 1.0
            db.insert("vectorised", type="semantic", embedding=vec,
                      tags=["vec"])
            v = db.search(embedding=vec, top_k=1,
                          include_embeddings=True)["records"][0]
            self.assertIn("embedding", v)

            # track_usage=False must not bump the counters forget scores on
            before = db.get(db.search(tags=["flag"])["records"][0]["id"])
            n = before["record"].get("recall_count")
            db.search(tags=["flag"], track_usage=False)
            after = db.get(db.search(tags=["flag"], track_usage=False)
                           ["records"][0]["id"], track_usage=False)
            self.assertEqual(after["record"].get("recall_count"), n)

    def test_count_and_max_importance(self):
        with Server() as srv, AegisClient(port=srv.port) as db:
            db.insert("low", type="semantic", tags=["c"], importance=0.1)
            db.insert("high", type="semantic", tags=["c"], importance=0.9)
            self.assertEqual(db.count(tags=["c"])["count"], 2)
            self.assertEqual(db.count(tags=["c"], max_importance=0.5)["count"], 1)
            self.assertEqual(db.count(type="episodic")["count"], 0)

    def test_working_memory_and_promote(self):
        with Server() as srv, AegisClient(port=srv.port) as db:
            w = db.insert("debugging auth", type="working",
                          session_id="s1", ttl_ms=60000)["record"]
            got = db.promote(w["id"], to_type="semantic", session_id="s1")
            self.assertTrue(got["ok"], got)

    def test_relate_and_traverse(self):
        with Server() as srv, AegisClient(port=srv.port,
                                          agent_id="ns") as db:
            a = db.insert("a", type="semantic")["record"]["id"]
            b = db.insert("b", type="semantic")["record"]["id"]
            c = db.insert("c", type="semantic")["record"]["id"]
            db.relate(a, b, "supersedes")
            db.relate(a, c, "derived_from")

            # The start record is included, at depth 0 — so a->{b,c} is three.
            out = db.traverse(a, depth=1)["records"]
            self.assertEqual([r["id"] for r in out], [a, b, c], out)
            self.assertEqual(out[0]["traversal"]["depth"], 0)

            # kinds narrows the walk, and the hop that reached each record is
            # reported, which is what makes a walk read as a path.
            only = db.traverse(a, depth=1, kinds=["supersedes"])["records"]
            self.assertEqual([r["id"] for r in only], [a, b], only)
            self.assertEqual(only[1]["traversal"]["via_kind"], "supersedes")
            self.assertEqual(only[1]["traversal"]["via_direction"], "out")
            # backwards, which needs the reverse edge index
            back = db.traverse(b, direction="in", kinds=["supersedes"])["records"]
            self.assertEqual([r["id"] for r in back], [b, a], back)
            self.assertEqual(back[1]["traversal"]["via_direction"], "in")

    def test_typed_facts_pattern_and_conflicts(self):
        reg = _registry()
        try:
            with Server(extra=["--predicate-registry", reg, "--inference",
                               "--inference-interval-sec", "1"]) as srv, \
                    AegisClient(port=srv.port) as db:
                hnsw = db.insert("hnsw.c", type="semantic",
                                 tags=["entity"])["record"]["id"]
                layer = db.insert("the storage layer", type="semantic",
                                  tags=["entity"])["record"]["id"]
                db.insert("hnsw.c is part of the storage layer", type="semantic",
                          fact={"s": hnsw, "p": "part_of", "o": {"id": layer}})

                hits = db.search(pattern={"s": hnsw, "p": "part_of"})["records"]
                self.assertEqual(len(hits), 1, hits)
                self.assertEqual(hits[0]["fact"]["p"], "part_of")
                self.assertEqual(db.count(pattern={"p": "part_of"})["count"], 1)

                # an undeclared predicate is refused, not stored
                with self.assertRaises(Exception):
                    db.insert("nope", type="semantic",
                              fact={"s": hnsw, "p": "invented", "o": "x"})

                # two live values for a single-valued predicate -> a conflict
                db.insert("defaults to none", type="semantic",
                          fact={"s": hnsw, "p": "defaults_to", "o": "none"})
                db.insert("defaults to local", type="semantic",
                          fact={"s": hnsw, "p": "defaults_to", "o": "local"})
                deadline = time.time() + 20
                while time.time() < deadline:
                    listed = db.conflicts()
                    if listed["total"]:
                        break
                    time.sleep(0.25)
                self.assertEqual(listed["total"], 1, listed)
                self.assertEqual(listed["conflicts"][0]["reason"], "cardinality")
                # limit=0 counts without listing
                probe = db.conflicts(limit=0)
                self.assertEqual((probe["conflicts"], probe["total"]), ([], 1))
        finally:
            os.unlink(reg)

    def test_pattern_needs_the_fact_index(self):
        with Server(extra=["--no-fact-index"]) as srv, \
                AegisClient(port=srv.port) as db:
            with self.assertRaises(NotReady):
                db.search(pattern={"p": "part_of"})

    def test_query_needs_the_lexical_index(self):
        with Server(extra=["--no-lexical-index"]) as srv, \
                AegisClient(port=srv.port) as db:
            with self.assertRaises(NotReady):
                db.search(query="anything")

    def test_consolidate_and_forget(self):
        with Server() as srv, AegisClient(port=srv.port) as db:
            for _ in range(3):
                db.insert("the deploy runbook lives in ops/deploy.md",
                          type="semantic", tags=["dup"], importance=0.5)
            res = db.consolidate(min_similarity=0.9)
            self.assertIn("merged", res)

            db.insert("transient", type="episodic", importance=0.01)
            dry = db.forget(min_retention=0.9, dry_run=True)
            self.assertIn("forgotten", dry)
            self.assertEqual(db.count(type="episodic")["count"], 1,
                             "dry_run must not delete")
            db.forget(min_retention=0.9, max_forget=10, usage_weight=0.0,
                      half_life_ms=1000)
            self.assertEqual(db.count(type="episodic")["count"], 0)

    def test_export_and_purge(self):
        with Server() as srv, AegisClient(port=srv.port) as db:
            db.insert("theirs", type="semantic", agent_id="other")
            for i in range(3):
                db.insert(f"mine {i}", type="semantic", agent_id="subject")

            dump = db.export(agent_id="subject")
            self.assertEqual(len(dump["records"]), 3, dump)
            page = db.export(agent_id="subject", limit=1)
            self.assertEqual(len(page["records"]), 1)
            nxt = db.export(agent_id="subject", limit=5,
                            after_id=page["records"][0]["id"])
            self.assertEqual(len(nxt["records"]), 2)

            self.assertTrue(db.purge(agent_id="subject", dry_run=True)["ok"])
            self.assertEqual(db.count(agent_id="subject")["count"], 3,
                             "dry_run must not purge")
            db.purge(agent_id="subject", compact=True)
            self.assertEqual(db.count(agent_id="subject")["count"], 0)
            self.assertEqual(db.count(agent_id="other")["count"], 1,
                             "a co-tenant survives")

    def test_snapshot(self):
        with Server() as srv, AegisClient(port=srv.port) as db:
            db.insert("something to back up", type="semantic")
            self.assertTrue(db.snapshot()["ok"])

    def test_tokens_and_scopes(self):
        with Server(token_lines=["adm", "ro-tok tenant ro"]) as srv:
            with AegisClient(port=srv.port, token="adm") as admin:
                listed = admin.token_list()["tokens"]
                self.assertGreaterEqual(len(listed), 2, listed)
                admin.token_add("new-tok", namespace="extra", scope="rw")
                self.assertGreater(len(admin.token_list()["tokens"]),
                                   len(listed))
                added = [t for t in admin.token_list()["tokens"]
                         if t.get("namespace") == "extra"][0]
                # A string fingerprint, not a number. Coercing it to an int
                # here is the bug this assertion exists to keep out.
                self.assertIsInstance(added["id"], str)
                self.assertTrue(admin.token_revoke(added["id"])["revoked"])
                self.assertNotIn("extra", [t.get("namespace") for t
                                           in admin.token_list()["tokens"]])

            # a read-only token cannot write, and a wrong one cannot do anything
            with AegisClient(port=srv.port, token="ro-tok") as ro:
                with self.assertRaises(Forbidden):
                    ro.insert("nope", type="semantic")
            with AegisClient(port=srv.port, token="wrong") as bad:
                with self.assertRaises(Unauthorized):
                    bad.stats()

    def test_agent_id_isolates(self):
        with Server() as srv:
            with AegisClient(port=srv.port, agent_id="a") as a, \
                    AegisClient(port=srv.port, agent_id="b") as b:
                a.insert("a's memory", type="semantic", tags=["t"])
                self.assertEqual(a.count(tags=["t"])["count"], 1)
                self.assertEqual(b.count(tags=["t"])["count"], 0)


if __name__ == "__main__":
    unittest.main()
