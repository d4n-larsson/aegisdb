"""Unit tests for grounding (ROADMAP 5.4 §4).

The threshold is the whole design, so most of these are about which of the two
errors the module commits when it is unsure. It must prefer fragmentation —
minting a second record for one thing — over conflation, because a wrong
resolution writes facts about the wrong entity and 5.3 then derives more of
them with nothing able to notice.
"""
import os
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.grounding import (ENTITY_TAG, GroundingResult, ground_mentions,
                                 looks_like_identifier, mint, normalize,
                                 resolve)


def _blend(sim, importance=0.5, confidence=1.0):
    """Exactly what tools.score_record returns, and the reason this fake exists.

    The score a caller sees is NOT a cosine: it is
    `sim * (0.5 + 0.5 * importance) * confidence`. An earlier version of this
    fake handed back raw cosines, which made a 0.85 floor look reachable — while
    against the real search an entity record minted at importance 0.5 tops out
    at `sim * 0.75`, so the floor could never be met and every paraphrase would
    have minted. A fake that is kinder than production hides precisely the bug
    it should be catching.
    """
    return round(sim * (0.5 + 0.5 * importance) * confidence, 4)


class FakeTools:
    """A store of entity records with scripted *cosines*.

    `scores` maps a mention to the (text, id, cosine) rows a semantic search
    matches; `lexical_rows` to what a lexical one returns. Kept separate on
    purpose: the two searches score on different scales, and a fake returning
    one set for both would hide that.
    """

    def __init__(self, scores=None, lexical_rows=None, next_id=100,
                 save_fails=False, embeddings=True, importance=0.5,
                 kind="semantic"):
        self.scores = scores or {}
        self.lexical_rows = lexical_rows or {}
        self.next_id = next_id
        self.saved = []
        self.searches = []
        self.save_fails = save_fails
        self.embeddings = embeddings
        self.importance = importance
        self.kind = kind

    def _embeddings_usable(self):
        return self.embeddings

    def search(self, query=None, tags=None, match="any", top_k=None,
               kind=None, lexical=False, **kw):
        self.searches.append({"query": query, "tags": list(tags or []),
                              "lexical": lexical, "top_k": top_k})
        rows = (self.lexical_rows if lexical else self.scores).get(query, [])
        out = []
        for (t, i, sim) in rows:
            mem = {"id": i, "text": t, "kind": self.kind,
                   "importance": self.importance, "confidence": 1.0}
            # A lexical result is server-ranked on a different scale entirely;
            # the exact pass must not read it, so give it a value that would
            # fail any cosine comparison.
            mem["score"] = 0.02 if lexical else _blend(sim, self.importance)
            out.append(mem)
        return {"ok": True, "memories": out}

    def save(self, text=None, tags=None, semantic=False, importance=0.5):
        if self.save_fails:
            return {"ok": False, "error": "nope"}
        self.saved.append({"text": text, "tags": list(tags or []),
                           "semantic": semantic})
        self.next_id += 1
        return {"ok": True, "id": self.next_id}


def cfg(**kw):
    base = dict(grounding_min_score=0.85, grounding_top_k=5,
                grounding_max_mint=8)
    base.update(kw)
    return SimpleNamespace(**base)


class TestNormalize(unittest.TestCase):
    def test_casefold_and_whitespace_only(self):
        self.assertEqual(normalize("  The  Recall Hook "), "the recall hook")

    def test_punctuation_is_preserved(self):
        """Stripping it would let `hnsw.c` and `hnsw c` collide, and collision
        is the expensive error."""
        self.assertEqual(normalize("hnsw.c:214"), "hnsw.c:214")

    def test_trailing_sentence_punctuation_is_dropped(self):
        """An extractor quoting a mention out of prose carries the full stop
        with it, and "recall hook." never matching "recall hook" is a
        fragmentation with no upside. An identifier's internal dot survives —
        it is not trailing."""
        self.assertEqual(normalize("recall hook."), "recall hook")
        self.assertEqual(normalize("hnsw.c"), "hnsw.c")

    def test_none_is_not_a_crash(self):
        """record_to_memory always sets "text", sometimes to None, so a `.get`
        default never fires — and None.split() would escape into a capture path
        documented as never raising."""
        self.assertEqual(normalize(None), "")


class TestIdentifierDetection(unittest.TestCase):
    def test_file_and_line_is_an_identifier(self):
        self.assertTrue(looks_like_identifier("hnsw.c:214"))
        self.assertTrue(looks_like_identifier("src/hnsw.c"))
        self.assertTrue(looks_like_identifier("v2"))

    def test_prose_is_not(self):
        self.assertFalse(looks_like_identifier("the storage layer"))
        self.assertFalse(looks_like_identifier("the neighbour selection loop"))

    def test_slightly_technical_prose_is_still_prose(self):
        """An earlier version accepted any <=3-word mention with a dot or a
        digit, which swept these up and then permanently barred each from
        resolving by similarity."""
        for prose in ("the 5.2 design", "HNSW v2", "Postgres 16",
                      "recall hook."):
            self.assertFalse(looks_like_identifier(prose), prose)


class TestResolve(unittest.TestCase):
    def test_exact_lexical_match_wins(self):
        t = FakeTools(lexical_rows={"the storage layer":
                                    [("The storage layer", 7, 0.1)]})
        self.assertEqual(resolve(t, "the storage layer", cfg()), 7)

    def test_prose_above_the_floor_resolves(self):
        t = FakeTools(scores={"the storage subsystem":
                              [("The storage layer", 7, 0.9)]})
        self.assertEqual(resolve(t, "the storage subsystem", cfg()), 7)

    def test_prose_below_the_floor_does_not(self):
        """A near-miss mints. Conflation writes facts about the wrong entity
        and inference compounds them; fragmentation only loses inferences and
        consolidate can undo it."""
        t = FakeTools(scores={"the query planner":
                              [("The storage layer", 7, 0.80)]})
        self.assertIsNone(resolve(t, "the query planner", cfg()))

    def test_an_identifier_never_resolves_by_similarity(self):
        """`hnsw.c:214` and `hnsw.c:215` are one edit apart and are different
        things. An identifier matches exactly or not at all."""
        t = FakeTools(scores={"hnsw.c:215": [("hnsw.c:214", 7, 0.99)]})
        self.assertIsNone(resolve(t, "hnsw.c:215", cfg()))

    def test_an_identifier_still_resolves_exactly(self):
        t = FakeTools(lexical_rows={"hnsw.c:214": [("hnsw.c:214", 7, 0.02)]})
        self.assertEqual(resolve(t, "hnsw.c:214", cfg()), 7)

    def test_the_lexical_pass_is_not_scored(self):
        """Fused scores are on the reciprocal-rank scale, so a cosine floor
        applied to them would admit or discard almost everything. The exact
        pass must ignore the score entirely — the fake gives lexical rows a
        value far below the floor and the match must still count."""
        t = FakeTools(lexical_rows={"the storage layer":
                                    [("The storage layer", 7, 0.001)]})
        self.assertEqual(resolve(t, "the storage layer", cfg()), 7)

    def test_an_entity_minted_at_the_default_importance_can_be_reused(self):
        """THE regression. score_record blends importance in, so a record
        minted at 0.5 scores at most `sim * 0.75`. Comparing a 0.85 cosine
        floor against that value directly makes reuse impossible, every
        paraphrase mints, and the store fragments with nothing to point at."""
        t = FakeTools(scores={"the storage subsystem":
                              [("The storage layer", 7, 0.95)]},
                      importance=0.5)
        self.assertEqual(resolve(t, "the storage subsystem", cfg()), 7)

    def test_scoreless_results_are_not_resolved(self):
        """Embeddings off means no cosine, and no cosine means no basis for
        reuse — mint rather than guess."""
        t = FakeTools()
        t.scores = {"the layer": []}
        t.search = lambda **kw: {"ok": True, "memories": [
            {"id": 7, "text": "something else", "score": None}]}
        self.assertIsNone(resolve(t, "the layer", cfg()))


    def test_a_record_with_no_text_does_not_crash(self):
        t = FakeTools()
        t.search = lambda **kw: {"ok": True, "memories": [
            {"id": 7, "text": None, "kind": "semantic", "score": None}]}
        self.assertIsNone(resolve(t, "the layer", cfg()))

    def test_embeddings_off_skips_the_semantic_pass(self):
        """It cannot resolve anything without a cosine, so issuing it is one
        wasted round-trip per mention on a long transcript."""
        t = FakeTools(embeddings=False)
        resolve(t, "the storage layer", cfg())
        self.assertEqual([s["lexical"] for s in t.searches], [True])

    def test_an_episodic_record_tagged_entity_is_not_an_entity(self):
        """`kind` is a server-side filter older servers ignore, and search
        documents that callers relying on it must re-check client-side.
        Otherwise an episodic record becomes a fact's subject."""
        t = FakeTools(lexical_rows={"the storage layer":
                                    [("the storage layer", 7, 0.1)]},
                      kind="episodic")
        self.assertIsNone(resolve(t, "the storage layer", cfg()))

    def test_entity_scoped_search(self):
        t = FakeTools()
        resolve(t, "the storage layer", cfg())
        self.assertTrue(all(s["tags"] == [ENTITY_TAG] for s in t.searches))

    def test_a_failed_search_resolves_to_nothing(self):
        t = FakeTools()
        t.search = lambda **kw: {"ok": False, "error": "down"}
        self.assertIsNone(resolve(t, "the storage layer", cfg()))


class TestGroundMentions(unittest.TestCase):
    def test_mints_what_it_cannot_resolve(self):
        t = FakeTools()
        res = ground_mentions(t, ["the storage layer"], cfg())
        self.assertEqual(res.minted, 1)
        self.assertEqual(res.resolved, 0)
        self.assertEqual(t.saved[0]["tags"], [ENTITY_TAG])
        self.assertTrue(t.saved[0]["semantic"])
        self.assertEqual(res.ids["the storage layer"], 101)

    def test_reuses_what_it_can(self):
        t = FakeTools(lexical_rows={"the storage layer":
                                    [("the storage layer", 7, 0.1)]})
        res = ground_mentions(t, ["the storage layer"], cfg())
        self.assertEqual((res.resolved, res.minted), (1, 0))
        self.assertEqual(t.saved, [])

    def test_the_same_mention_twice_is_one_entity(self):
        t = FakeTools()
        res = ground_mentions(t, ["the layer", "the layer"], cfg())
        self.assertEqual(res.minted, 1)
        self.assertEqual(len(t.saved), 1)

    def test_two_spellings_of_one_mention_collapse_before_searching(self):
        """Keyed on the raw string these were two mentions: two extra
        round-trips, and a second minted entity whenever the lexical index had
        not yet caught the write moments earlier."""
        t = FakeTools()
        res = ground_mentions(t, ["The Storage Layer", "the storage layer"],
                              cfg())
        self.assertEqual(res.minted, 1)
        self.assertEqual(len(t.saved), 1)
        self.assertEqual(res.ids["The Storage Layer"],
                         res.ids["the storage layer"])

    def test_minting_is_capped_and_the_overflow_is_reported(self):
        """Past the cap a mention is reported unresolved rather than guessed
        at: the caller drops one triple, where a wrong resolution would cost
        every conclusion drawn from it."""
        t = FakeTools()
        res = ground_mentions(t, [f"thing {i}" for i in range(5)],
                              cfg(grounding_max_mint=2))
        self.assertEqual(res.minted, 2)
        self.assertEqual(len(res.unresolved), 3)
        self.assertEqual(len(t.saved), 2)

    def test_a_failed_save_is_unresolved_not_silently_dropped(self):
        t = FakeTools(save_fails=True)
        res = ground_mentions(t, ["the layer"], cfg())
        self.assertEqual((res.resolved, res.minted), (0, 0))
        self.assertEqual(res.unresolved, ["the layer"])

    def test_blank_mentions_are_skipped(self):
        t = FakeTools()
        res = ground_mentions(t, ["", "   "], cfg())
        self.assertEqual(res.ids, {})
        self.assertEqual(t.saved, [])

    def test_mint_rate_is_the_leading_indicator(self):
        """A store minting for every mention has a threshold problem that would
        otherwise surface only as a slowly fragmenting graph."""
        t = FakeTools(lexical_rows={"known": [("known", 7, 0.1)]})
        res = ground_mentions(t, ["known", "unknown"], cfg())
        self.assertEqual(res.mint_rate, 0.5)
        self.assertEqual(GroundingResult().mint_rate, 0.0)


if __name__ == "__main__":
    unittest.main()
