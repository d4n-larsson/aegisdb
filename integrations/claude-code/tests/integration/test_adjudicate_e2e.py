"""Adjudication end to end (ROADMAP 5.4 §6, design contract test 7).

The claim under test is narrow and worth stating plainly: the rules find the
contradiction, the model resolves only that one pair, and the verdict lands as
a **supersession** — not an edit, and not a rewrite of either fact. So these
run against a real aegisdb with `--inference`, because the pair has to come
from the server's own detection rather than from a fixture: a test that handed
the adjudicator a pair it made up would prove the model call works and nothing
about whether the two halves meet.
"""
import json
import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from harness import AegisServer, binary_available, make_config  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from aegis_mcp.adjudicate import adjudicate_conflicts  # noqa: E402
from aegis_mcp.client import AegisClient  # noqa: E402
from aegis_mcp.embeddings import FakeProvider  # noqa: E402
from aegis_mcp.extract import FakeExtractionProvider  # noqa: E402
from aegis_mcp.tools import MemoryTools  # noqa: E402

REGISTRY = {"defaults_to": {"object": "string", "cardinality": "one"}}
STALE = FakeExtractionProvider.STALE_MARKER


def _registry_file():
    fd, path = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w") as fh:
        json.dump(REGISTRY, fh)
    return path


@unittest.skipUnless(binary_available(), "aegisdb binary not built")
class TestAdjudicateE2E(unittest.TestCase):
    def setUp(self):
        self.registry = _registry_file()

    def tearDown(self):
        os.unlink(self.registry)

    def _server(self):
        return AegisServer(extra_args=[
            "--predicate-registry", self.registry, "--inference",
            "--inference-interval-sec", "1"])

    def _wired(self, srv, **overrides):
        opts = {"adjudicate_conflicts": True}
        opts.update(overrides)
        cfg = make_config(srv, **opts)
        tools = MemoryTools(cfg, AegisClient.from_config(cfg),
                            FakeProvider(srv.dim))
        return cfg, tools

    def _contradiction(self, tools, loser_suffix="", winner_suffix=""):
        """Two live values for a single-valued predicate, and wait for the job.

        Returns (subject_id, first_id, second_id) once the server itself has
        flagged the pair — not once the records exist.
        """
        subj = tools.save("the recall hook", tags=["entity"],
                          semantic=True)["id"]
        a = tools.save(f"the recall hook defaults to none{loser_suffix}",
                       tags=["fact"], semantic=True,
                       fact={"s": subj, "p": "defaults_to", "o": "none"})["id"]
        b = tools.save(f"the recall hook defaults to local{winner_suffix}",
                       tags=["fact"], semantic=True,
                       fact={"s": subj, "p": "defaults_to", "o": "local"})["id"]
        deadline = time.time() + 20
        while time.time() < deadline:
            listed = tools.conflicts()
            if listed.get("ok") and listed.get("total"):
                return subj, a, b
            time.sleep(0.25)
        self.fail("the inference job never flagged the contradiction")

    def test_a_verdict_is_a_supersession_not_an_edit(self):
        with self._server() as srv:
            cfg, tools = self._wired(srv)
            _, loser, winner = self._contradiction(tools, loser_suffix=STALE)

            before = tools.get(winner)["memory"]
            res = adjudicate_conflicts(tools, cfg, FakeExtractionProvider())
            self.assertEqual(res.resolved, 1, f"{res}")
            self.assertEqual(res.abstained, 0)

            # The loser is tombstoned, the winner untouched. "Not an edit" is
            # the whole point: the model's judgment became a record, and
            # neither fact was rewritten to agree with it.
            self.assertFalse(tools.get(loser).get("ok"),
                             "the superseded fact is tombstoned")
            after = tools.get(winner)["memory"]
            self.assertTrue(after.get("ok", True))
            self.assertEqual(after["text"], before["text"],
                             "the surviving fact's prose is unchanged")
            self.assertEqual(after["fact"], before["fact"],
                             "and so is its triple")

            # And the supersession is on the record, walkable, so the verdict
            # is auditable rather than an unexplained deletion.
            raw = tools._request({"operation": "get", "id": winner})
            edges = [e for e in raw["record"].get("relationships", [])
                     if e["kind"] == "supersedes"]
            self.assertEqual([e["to_id"] for e in edges], [loser],
                             f"the winner records what it superseded: {edges}")

    def test_the_conflict_stops_being_reported(self):
        with self._server() as srv:
            cfg, tools = self._wired(srv)
            self._contradiction(tools, loser_suffix=STALE)
            adjudicate_conflicts(tools, cfg, FakeExtractionProvider())

            deadline = time.time() + 20
            while time.time() < deadline:
                if tools.conflicts().get("total") == 0:
                    break
                time.sleep(0.25)
            self.assertEqual(tools.conflicts()["total"], 0,
                             "a resolved contradiction leaves the list")

    def test_neither_leaves_the_conflict_reported(self):
        """The branch that has to stay safe.

        Neither side is marked, so the fake abstains — which is also what an
        unreachable backend, an unparseable reply and an unsure model all do.
        Nothing may be deleted, and the contradiction must still be there for
        the next run (or a human) to deal with.
        """
        with self._server() as srv:
            cfg, tools = self._wired(srv)
            _, a, b = self._contradiction(tools)

            res = adjudicate_conflicts(tools, cfg, FakeExtractionProvider())
            self.assertEqual(res.considered, 1, f"{res}")
            self.assertEqual(res.abstained, 1)
            self.assertEqual(res.resolved, 0)
            self.assertTrue(tools.get(a).get("ok"), "both facts survive")
            self.assertTrue(tools.get(b).get("ok"))
            self.assertEqual(tools.conflicts()["total"], 1,
                             "and the contradiction is still reported")

    def test_off_by_default_changes_nothing(self):
        with self._server() as srv:
            cfg, tools = self._wired(srv, adjudicate_conflicts=False)
            _, a, b = self._contradiction(tools, loser_suffix=STALE)

            res = adjudicate_conflicts(tools, cfg, FakeExtractionProvider())
            self.assertEqual((res.seen, res.considered, res.resolved),
                             (0, 0, 0), f"{res}")
            self.assertTrue(tools.get(a).get("ok"))
            self.assertTrue(tools.get(b).get("ok"))

    def test_a_star_of_conflicts_costs_one_verdict_per_record(self):
        """Three values for a single-valued predicate is three flagged pairs
        sharing records. Once a record is tombstoned every pair naming it is
        stale, and re-putting those to the model would spend calls to be told
        what the deletion already settled."""
        with self._server() as srv:
            cfg, tools = self._wired(srv)
            subj = tools.save("the recall hook", tags=["entity"],
                              semantic=True)["id"]
            ids = []
            for val in ("none", "local", "voyage"):
                suffix = STALE if val != "none" else ""
                ids.append(tools.save(
                    f"the recall hook defaults to {val}{suffix}",
                    tags=["fact"], semantic=True,
                    fact={"s": subj, "p": "defaults_to", "o": val})["id"])
            deadline = time.time() + 20
            while time.time() < deadline:
                if (tools.conflicts().get("total") or 0) >= 2:
                    break
                time.sleep(0.25)

            res = adjudicate_conflicts(tools, cfg, FakeExtractionProvider())
            # `none` is the only unmarked one, so it wins both pairs it is in.
            self.assertTrue(tools.get(ids[0]).get("ok"),
                            "the unmarked fact survives")
            self.assertEqual(res.considered + res.skipped, res.seen,
                             f"every pair is accounted for exactly once: {res}")
            self.assertGreaterEqual(res.resolved, 1, f"{res}")


if __name__ == "__main__":
    unittest.main()
