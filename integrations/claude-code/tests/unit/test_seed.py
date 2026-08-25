"""Unit tests for `aegisdb-seed`: corpus discovery, the bundled vocabulary, and
the two-pass label-to-id seeding."""
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.config import PROJECT_DIR
from aegis_mcp.init import ensure_predicates
from aegis_mcp.seed import (DEFAULT_PREDICATES, discover_corpora, facts_dir,
                            load_registry, registry_path, seed_corpus)

REGISTRY = {"part_of": {"object": "id"}, "defaults_to": {"object": "string"}}

CORPUS = {
    "name": "t",
    "entities": {"a": "the a thing", "b": "the b thing"},
    "facts": [
        ["a", "part_of", "b", "a is part of b."],
        ["b", "defaults_to", "42", "b defaults to 42."],
    ],
}


class FakeClient:
    """Records every payload and answers searches from what it has 'stored'."""

    def __init__(self, existing=None):
        self.sent = []
        self.records = list(existing or [])
        self._next = 100

    def request(self, payload):
        self.sent.append(payload)
        op = payload["operation"]
        if op == "search":
            if payload.get("pattern") is not None:
                pat = payload["pattern"]
                hits = [r for r in self.records if r.get("fact") == pat]
                return {"ok": True, "records": hits}
            want = payload.get("query")
            return {"ok": True,
                    "records": [r for r in self.records
                                if r.get("data") == want and "entity" in (r.get("tags") or [])]}
        if op == "insert":
            rec = {"id": self._next, "data": payload["data"],
                   "tags": payload.get("tags") or []}
            if payload.get("fact"):
                rec["fact"] = payload["fact"]
            self._next += 1
            self.records.append(rec)
            return {"ok": True, "record": rec}
        raise AssertionError(f"unexpected op {op}")

    def inserts(self):
        return [p for p in self.sent if p["operation"] == "insert"]


class TestDiscovery(unittest.TestCase):
    def _project(self, *corpus_names):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        if corpus_names:
            os.makedirs(facts_dir(env={}, cwd=d))
            for n in corpus_names:
                with open(os.path.join(facts_dir(env={}, cwd=d), n), "w") as fh:
                    json.dump({"name": n}, fh)
        return d

    def test_no_directory_is_empty_not_an_error(self):
        self.assertEqual(discover_corpora(env={}, cwd=self._project()), [])

    def test_sorted_so_a_run_is_reproducible(self):
        d = self._project("b.json", "a.json", "c.json")
        got = [os.path.basename(p) for p in discover_corpora(env={}, cwd=d)]
        self.assertEqual(got, ["a.json", "b.json", "c.json"])

    def test_only_json_is_picked_up(self):
        d = self._project("a.json")
        open(os.path.join(facts_dir(env={}, cwd=d), "notes.md"), "w").close()
        self.assertEqual(len(discover_corpora(env={}, cwd=d)), 1)

    def test_registry_path_is_project_local(self):
        d = self._project()
        self.assertEqual(registry_path(env={}, cwd=d),
                         os.path.join(d, PROJECT_DIR, "predicates.json"))


class TestBundledVocabulary(unittest.TestCase):
    def test_it_exists_and_declares_object_kinds(self):
        reg = json.load(open(DEFAULT_PREDICATES))
        self.assertTrue(reg)
        for name, spec in reg.items():
            self.assertIn(spec.get("object"), ("id", "string"), name)

    def test_load_registry_falls_back_to_it(self):
        with tempfile.TemporaryDirectory() as d:
            reg, used = load_registry(os.path.join(d, "nope.json"))
            self.assertEqual(reg, json.load(open(DEFAULT_PREDICATES)))
            self.assertEqual(used, DEFAULT_PREDICATES)

    def test_a_named_registry_that_is_missing_is_an_error(self):
        """A typo'd --registry must not silently seed against the fallback."""
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit):
                load_registry(os.path.join(d, "typo.json"), explicit=True)

    def test_it_matches_the_repo_root_example(self):
        """The packaged copy and predicates.example.json must not drift.

        Skipped when the repo isn't around (an installed wheel), which is
        exactly when there is nothing to drift from.
        """
        here = os.path.abspath(__file__)          # …/integrations/claude-code/tests/unit/
        root = here
        for _ in range(5):                        # unit, tests, claude-code, integrations, repo
            root = os.path.dirname(root)
        example = os.path.join(root, "predicates.example.json")
        if not os.path.isfile(example):
            self.skipTest("no repo checkout")
        self.assertEqual(json.load(open(DEFAULT_PREDICATES)), json.load(open(example)))

    def test_init_writes_it_then_keeps_yours(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIn("written", ensure_predicates(d))
            path = os.path.join(d, PROJECT_DIR, "predicates.json")
            with open(path, "w") as fh:
                json.dump({"mine": {"object": "string"}}, fh)
            self.assertIn("kept", ensure_predicates(d))
            self.assertEqual(json.load(open(path)), {"mine": {"object": "string"}})


class TestSeedCorpus(unittest.TestCase):
    def test_labels_become_ids_and_object_kind_follows_the_registry(self):
        db = FakeClient()
        rep = seed_corpus(db, CORPUS, REGISTRY)
        self.assertEqual((rep["entities_created"], rep["facts_written"]), (2, 2))
        facts = [p["fact"] for p in db.inserts() if p.get("fact")]
        self.assertEqual(facts[0], {"s": 100, "p": "part_of", "o": {"id": 101}})
        self.assertEqual(facts[1], {"s": 101, "p": "defaults_to", "o": "42"})

    def test_an_entity_is_reused_by_exact_prose(self):
        db = FakeClient([{"id": 7, "data": "the a thing", "tags": ["entity"]}])
        rep = seed_corpus(db, CORPUS, REGISTRY)
        self.assertEqual((rep["entities_created"], rep["entities_reused"]), (1, 1))
        self.assertEqual(db.inserts()[1]["fact"]["s"], 7)

    def test_near_prose_is_not_reused(self):
        db = FakeClient([{"id": 7, "data": "the a thing!", "tags": ["entity"]}])
        self.assertEqual(seed_corpus(db, CORPUS, REGISTRY)["entities_reused"], 0)

    def test_rerunning_writes_nothing(self):
        db = FakeClient()
        seed_corpus(db, CORPUS, REGISTRY)
        before = len(db.inserts())
        rep = seed_corpus(db, CORPUS, REGISTRY)
        self.assertEqual(rep["facts_present"], 2)
        self.assertEqual(rep["entities_reused"], 2)
        self.assertEqual(len(db.inserts()), before)

    def test_dry_run_writes_nothing_and_still_counts(self):
        db = FakeClient()
        rep = seed_corpus(db, CORPUS, REGISTRY, dry_run=True)
        self.assertEqual((rep["entities_created"], rep["facts_written"]), (2, 2))
        self.assertEqual(db.inserts(), [])

    def test_an_undeclared_predicate_is_refused_not_sent(self):
        corpus = dict(CORPUS, facts=[["a", "invented_by", "b", "x"]])
        db = FakeClient()
        rep = seed_corpus(db, corpus, REGISTRY)
        self.assertEqual(rep["facts_written"], 0)
        self.assertEqual(rep["refused"][0][0], "invented_by")
        self.assertTrue(all(not p.get("fact") for p in db.inserts()))

    def test_an_unknown_entity_label_is_refused(self):
        corpus = dict(CORPUS, facts=[["a", "part_of", "nope", "x"]])
        rep = seed_corpus(FakeClient(), corpus, REGISTRY)
        self.assertIn("unknown entity", rep["refused"][0][1])

    def test_a_malformed_row_is_refused_not_crashed(self):
        corpus = dict(CORPUS, facts=[["a", "part_of"]])
        rep = seed_corpus(FakeClient(), corpus, REGISTRY)
        self.assertIn("not a [s, p, o, prose] row", rep["refused"][0][1])

    def test_namespace_is_sent_on_every_write(self):
        db = FakeClient()
        seed_corpus(db, CORPUS, REGISTRY, namespace="ns")
        self.assertTrue(all(p.get("agent_id") == "ns" for p in db.inserts()))


if __name__ == "__main__":
    unittest.main()


class FailingClient:
    """A server that refuses the probe — e.g. --no-lexical-index."""

    def __init__(self, code="NOT_READY"):
        self.code = code
        self.inserted = 0

    def request(self, payload):
        if payload["operation"] == "search":
            return {"ok": False, "error": {"code": self.code,
                                           "message": "lexical index disabled"}}
        self.inserted += 1
        return {"ok": True, "record": {"id": self.inserted}}


class TestProbesMustActuallyRun(unittest.TestCase):
    def test_a_refused_lookup_is_not_read_as_absent(self):
        """Otherwise every run re-mints every entity: idempotency silently
        becomes duplication."""
        db = FailingClient()
        with self.assertRaises(SystemExit) as ctx:
            seed_corpus(db, CORPUS, REGISTRY)
        self.assertIn("NOT_READY", str(ctx.exception))
        self.assertEqual(db.inserted, 0)


class ExplodingClient(FakeClient):
    """Inserts fail; searches still work."""

    def request(self, payload):
        if payload["operation"] == "insert":
            self.sent.append(payload)
            return {"ok": False, "error": {"message": "QUOTA_EXCEEDED"}}
        return super().request(payload)


class TestWriteFailuresAreReportedNotRaised(unittest.TestCase):
    def test_a_failed_entity_insert_reports_instead_of_aborting(self):
        rep = seed_corpus(ExplodingClient(), CORPUS, REGISTRY)
        self.assertEqual(rep["entities_created"], 0)
        self.assertEqual(rep["facts_written"], 0)
        # every entity refused, and the facts that needed them refused in turn
        self.assertGreaterEqual(len(rep["refused"]), 2)
        self.assertTrue(any("entity insert failed" in why for _, why in rep["refused"]))
