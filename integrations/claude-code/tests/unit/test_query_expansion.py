"""Query expansion for the keyword-only recall path.

With embeddings off — the default — the keyword index is the only content-based
retrieval there is, and it matches whole tokens. So a memory was found only when
the prompt happened to reuse its exact words: *"how do I deploy this project?"*
missed *"widgetco deploys with make ship"* on the plural alone, which is most of
why recall felt empty out of the box.

What these pin is the shape of the trade. Expansion may only ever *add* terms,
must never touch an identifier, and must stay bounded — the keyword path earns
its place by matching `--tenant-max-records` and `hnsw.c:214` exactly, and a
change that blunts that has taken more than it gave.
"""
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.config import Config  # noqa: E402
from aegis_mcp.embeddings import FakeProvider, NoneProvider  # noqa: E402
from aegis_mcp.tools import (MemoryTools, _MAX_EXPANDED_TERMS,  # noqa: E402
                             expand_lexical_query)


def terms(query):
    """The terms actually sent to the keyword index, in order."""
    return expand_lexical_query(query).split()


def added(query):
    """Only the terms expansion introduced — the ones not in the question."""
    asked = {t.strip(".,;:!?\"'()[]{}").lower() for t in query.split()}
    return [t for t in terms(query) if t.lower() not in asked]


class TestWhatItAdds(unittest.TestCase):
    def test_the_case_that_prompted_it(self):
        self.assertIn("deploys", added("how do I deploy this project?"))

    def test_plurals_reach_the_singular(self):
        self.assertIn("guard", added("what guards the proxy?"))
        self.assertIn("query", added("queries running slowly"))

    def test_verb_forms_reach_the_stem(self):
        self.assertIn("warm", added("why is the cache warming"))
        self.assertIn("cache", added("the value was cached"))

    def test_a_doubled_consonant_is_undone(self):
        """English doubles the letter before -ing, and guessing wrong is the
        difference between finding "run" and finding nothing."""
        self.assertIn("run", added("what keeps running"))

    def test_the_content_of_the_question_survives(self):
        """Everything dropped is a function word; every term that carries
        meaning is still sent, and in the order it was asked."""
        asked = terms("why does hnsw.c:214 cap at 4096 exactly?")
        self.assertEqual([t for t in asked if t in ("hnsw.c:214", "cap", "4096")],
                         ["hnsw.c:214", "cap", "4096"])


class TestWhatItLeavesAlone(unittest.TestCase):
    def test_identifiers_are_untouched(self):
        """The property that makes the keyword path worth having: an exact flag,
        a file:line, an error code. Splitting these into word parts would add
        `tenants` and `hnsws` — noise at best, and a step toward mangling the
        thing being searched for."""
        for token in ("--tenant-max-records", "hnsw.c:214", "AEGIS_RECALL_TOP_K",
                      "v0.9.2", "0x7f", "test_recall.py::test_budget"):
            self.assertEqual(added(f"about {token} please"), added("about please"),
                             f"{token} was taken apart")

    def test_function_words_are_skipped(self):
        """"this" is not the plural of "thi"."""
        for junk in ("thi", "tha", "wha", "wher", "hav"):
            self.assertNotIn(junk, added("what is this that where have"))

    def test_double_s_words_are_not_depluralised(self):
        self.assertNotIn("acces", added("class access"))
        self.assertNotIn("clas", added("class access"))

    def test_short_words_are_left_out(self):
        self.assertEqual(added("is it up"), [])

    def test_nothing_to_add_returns_the_query_unchanged(self):
        self.assertEqual(expand_lexical_query(""), "")

    def test_a_question_of_nothing_but_function_words_is_sent_as_asked(self):
        """Dropping every term would leave no query at all, and an empty query
        is not a query."""
        self.assertEqual(expand_lexical_query("what is it"), "what is it")


class TestItDropsWhatCarriesNoSignal(unittest.TestCase):
    """The other half of the default being poor: the keyword index has no
    stopword list, so *any* question containing "the" matched *any* memory
    containing "the". On a small corpus IDF has nothing to push against, and
    recall injected an unrelated memory with a confident score."""

    def test_function_words_are_not_searched_on(self):
        self.assertNotIn("the", terms("what is the kubernetes ingress?"))
        self.assertNotIn("is", terms("what is the kubernetes ingress?"))

    def test_the_words_that_carry_the_question_remain(self):
        sent = terms("what is the kubernetes ingress annotation?")
        for word in ("kubernetes", "ingress", "annotation"):
            self.assertIn(word, sent)

    def test_identifiers_are_never_dropped(self):
        """They are the terms most worth matching exactly, and several are
        shorter than the length cut-off."""
        for token in ("hnsw.c:214", "--ann-threshold", "0x7f", "v0.9.2", "5544"):
            self.assertIn(token, terms(f"why does {token} do that"))


class TestItStaysBounded(unittest.TestCase):
    def test_a_long_prompt_cannot_explode_the_query(self):
        """Every added term is another posting list to walk."""
        self.assertLessEqual(
            len(added(" ".join(f"word{i}able" for i in range(200)))),
            _MAX_EXPANDED_TERMS)

    def test_duplicates_are_not_repeated(self):
        terms = added("deploy deploy deploying deployed")
        self.assertEqual(len(terms), len(set(terms)))


class _Spy:
    """Captures the payload that would go to the server."""

    def __init__(self):
        self.payload = None

    def request(self, payload, **kw):
        self.payload = payload
        return {"ok": True, "records": [], "total": 0}


class TestOnlyTheKeywordOnlyPathExpands(unittest.TestCase):
    def _sent(self, provider, dim=16):
        spy = _Spy()
        cfg = Config(embedding_mode="none" if isinstance(provider, NoneProvider)
                     else "local", embedding_dimensions=dim, namespace="t")
        MemoryTools(cfg, spy, provider).search(query="how do I deploy?",
                                               lexical=True)
        return spy.payload["query"]

    def test_without_embeddings_the_query_is_expanded(self):
        self.assertIn("deploys", self._sent(NoneProvider()))

    def test_with_embeddings_it_is_sent_verbatim(self):
        """The vector already spans the morphology; extra terms would only tilt
        the fusion the server does between the two sides."""
        self.assertEqual(self._sent(FakeProvider(16)), "how do I deploy?")


if __name__ == "__main__":
    unittest.main()
