"""Why `symbolic` is false, said where an operator can read it.

`ask_pattern` reads like a switch over the store, but a question is turned into
a pattern by a *model*, so the read path runs on the extraction backend. With
`extract_mode` at its default the feature is on in the config and inert at
runtime, and every result then carries `"symbolic": false` — indistinguishable
from "the corpus had no answer". These pin the log lines that tell the two
apart, in both directions: the complaint names the setting to change, and a
working read path says so rather than being inferred from the absence of a
complaint.
"""
import io
import os
import sys
import unittest
from contextlib import redirect_stderr
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from aegis_mcp.config import Config  # noqa: E402
from aegis_mcp.extract import (ExtractionProvider,  # noqa: E402
                               PredicateSpec)
from aegis_mcp.server import (_read_path,  # noqa: E402
                              read_path_note, search_or_ask)


class _Tools:
    def __init__(self, config):
        self.config = config


def run(config, vocab=None, vocab_ok=True):
    err = io.StringIO()
    with redirect_stderr(err):
        got = _read_path(_Tools(config), vocab, vocab_ok)
    return got, err.getvalue()


VOCAB = [PredicateSpec(name="guarded_by", object="id")]


class TestReadPathOffWithoutABackend(unittest.TestCase):
    def test_the_default_extract_mode_disables_it_and_says_so(self):
        got, log = run(Config(ask_pattern=True), VOCAB)
        self.assertEqual(got, (None, None))
        self.assertIn("AEGIS_EXTRACT_MODE", log)
        self.assertIn("AEGIS_ASK_PATTERN", log)
        self.assertIn("symbolic", log)

    def test_a_configured_but_unusable_backend_reads_differently(self):
        """"You set nothing" and "what you set isn't working" need different
        fixes, so they must not share a message.

        The provider is stubbed rather than chosen: whether `anthropic` is
        genuinely unavailable depends on the SDK and key on the machine running
        the tests, and this is about the message, not about that.
        """
        with mock.patch("aegis_mcp.extract.make_extraction_provider",
                        return_value=ExtractionProvider()):
            got, log = run(Config(ask_pattern=True, extract_mode="anthropic"),
                           VOCAB)
        self.assertEqual(got, (None, None))
        self.assertIn("anthropic", log)
        self.assertIn("unavailable", log)

    def test_verbalize_alone_names_its_own_setting(self):
        _, log = run(Config(ask_verbalize=True))
        self.assertIn("AEGIS_ASK_VERBALIZE", log)
        self.assertNotIn("AEGIS_ASK_PATTERN", log)

    def test_both_settings_off_stays_silent(self):
        """Strictly additive means costing nothing, log lines included."""
        got, log = run(Config())
        self.assertEqual(got, (None, None))
        self.assertEqual(log, "")


class TestReadPathOn(unittest.TestCase):
    def test_a_usable_backend_returns_the_vocabulary_and_the_provider(self):
        vocab, provider = run(Config(ask_pattern=True, extract_mode="fake"),
                              VOCAB)[0]
        self.assertEqual(vocab, VOCAB)
        self.assertTrue(provider.available())

    def test_an_unreadable_registry_disables_it_rather_than_guessing(self):
        """A configured vocabulary that could not be read is an operator error;
        answering against a different one would hide it."""
        self.assertEqual(run(Config(ask_pattern=True, extract_mode="fake"),
                             None, False)[0], (None, None))


if __name__ == "__main__":
    unittest.main()


class TestTheNoteTheCallerSees(unittest.TestCase):
    """`"symbolic": false` alone cannot distinguish a question the corpus
    cannot answer from a read path that never ran, and the caller does not read
    the server's stderr. The note closes that gap; it must appear only when
    something really is misconfigured."""

    def test_nothing_is_said_when_the_feature_is_off(self):
        self.assertIsNone(read_path_note(Config(), None, None))

    def test_nothing_is_said_when_it_is_wired_correctly(self):
        self.assertIsNone(read_path_note(Config(ask_pattern=True), VOCAB,
                                         ExtractionProvider()))

    def test_a_missing_backend_names_the_setting_to_change(self):
        note = read_path_note(Config(ask_pattern=True), VOCAB, None)
        self.assertIn("AEGIS_EXTRACT_MODE", note)
        self.assertIn("AEGIS_ASK_PATTERN", note)

    def test_a_missing_vocabulary_is_its_own_reason(self):
        """Different fix: the backend is fine, there is nothing to ask against."""
        note = read_path_note(Config(ask_pattern=True, extract_mode="fake"),
                              None, ExtractionProvider())
        self.assertIn("vocabulary", note)
        self.assertIn("AEGIS_EXTRACT_REGISTRY", note)

    def test_verbalize_alone_needs_no_vocabulary(self):
        self.assertIsNone(read_path_note(Config(ask_verbalize=True), None,
                                         ExtractionProvider()))


class _SearchTools:
    """Records the one call `search_or_ask` makes, and answers plausibly."""

    def __init__(self, config):
        self.config = config
        self.calls = []

    def search(self, **kw):
        self.calls.append(kw)
        return {"ok": True, "total": 0, "memories": []}


class TestSearchOrAsk(unittest.TestCase):
    def test_a_question_carries_the_note_when_the_read_path_is_off(self):
        tools = _SearchTools(Config(ask_pattern=True))
        res = search_or_ask(tools, None, None, "because reasons",
                            query="what guards the proxy?")
        self.assertEqual(res["read_path"], "because reasons")
        self.assertEqual(len(tools.calls), 1)

    def test_a_filtered_search_does_not(self):
        """It was never a candidate for the read path, so explaining why the
        read path did not run would be noise on an unrelated call."""
        tools = _SearchTools(Config(ask_pattern=True))
        res = search_or_ask(tools, None, None, "because reasons",
                            query="proxy", tags=["infra"])
        self.assertNotIn("read_path", res)

    def test_a_working_read_path_adds_nothing(self):
        tools = _SearchTools(Config())
        res = search_or_ask(tools, None, None, None, query="proxy")
        self.assertEqual(res, {"ok": True, "total": 0, "memories": []})

    def test_the_filters_still_reach_the_server(self):
        tools = _SearchTools(Config())
        search_or_ask(tools, None, None, None, query="q", tags=["a"],
                      match="all", start_time=1, end_time=2, top_k=9)
        self.assertEqual(tools.calls[0], {"query": "q", "tags": ["a"],
                                          "match": "all", "start_time": 1,
                                          "end_time": 2, "top_k": 9,
                                          "lexical": True})
