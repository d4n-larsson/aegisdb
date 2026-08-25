"""MCP server binding (US1 surface / T014).

Thin wrapper: it lazy-imports the optional ``mcp`` SDK and registers tools that
delegate straight to :class:`aegis_mcp.tools.MemoryTools`. All behaviour lives in
the core modules, so this file is just protocol glue — it is the only place that
requires ``mcp`` to be installed.

Both SDK majors are supported; see :func:`_server_class`.
"""
from __future__ import annotations

import sys

from .ask import ask, verbalize_all
from .client import AegisClient, check_startup
from .config import ConfigError, load_config
from .embeddings import make_provider
from .tools import MemoryTools


def _server_class():
    """The SDK's server class, across both major versions (None if unusable).

    mcp 2.0 renamed ``FastMCP`` to ``MCPServer`` and moved it to
    ``mcp.server.mcpserver``. Everything this module uses is identical on both —
    ``Cls(name)``, the ``@tool()`` decorator (same JSON Schema derived from the
    same type hints), and ``run()`` defaulting to stdio — and so is the wire
    result for a dict-returning tool, so supporting both costs one import
    fallback and spares anyone pinned to 1.x a forced upgrade.

    Prefer 2.x: a fresh install resolves there, so that is the path most users
    are on.
    """
    try:
        from mcp.server.mcpserver import MCPServer  # mcp >= 2.0
        return MCPServer
    except ImportError:
        pass
    try:
        from mcp.server.fastmcp import FastMCP  # mcp 1.x
        return FastMCP
    except ImportError:
        return None


def _new_server(server_cls, name: str):
    """Instantiate the SDK server, advertising *our* version where it's accepted.

    Only 2.x takes a ``version``; 1.x has no parameter and reports the SDK's own
    version instead, which was never what a client wanted to see. Passing the
    package version keeps `serverInfo` meaningful on 2.x, where the default is an
    empty string.
    """
    import inspect
    if "version" in inspect.signature(server_cls).parameters:
        try:
            from importlib.metadata import version
            return server_cls(name, version=version("aegisdb-mcp"))
        except Exception:  # noqa: BLE001 — running from a checkout, not installed
            pass
    return server_cls(name)


def _sdk_error() -> str:
    """Why the SDK is unusable — "missing" and "wrong version" need different fixes.

    An earlier release reported a too-new SDK as a missing package, which sent
    users off to install what they already had.
    """
    try:
        from importlib.metadata import version
        found = version("mcp")
    except Exception:  # noqa: BLE001 — diagnostics only, never mask the real error
        found = ""
    if found:
        return (f"[aegis-mcp] the installed 'mcp' package ({found}) provides "
                f"neither mcp.server.mcpserver.MCPServer (2.x) nor "
                f"mcp.server.fastmcp.FastMCP (1.x); this server needs "
                f"mcp>=1,<3: pip install 'mcp>=1,<3'")
    return ("[aegis-mcp] the 'mcp' package is required to run the server: "
            "pip install 'mcp>=1,<3'")


def build_tools(config=None) -> MemoryTools:
    config = config or load_config()
    client = AegisClient.from_config(config)
    provider = make_provider(config)

    # NOTE: provider/config embedding-dimension validation is deferred to the
    # first embed (MemoryTools._embeddings_usable), NOT done here. Reading a
    # local provider's dimension forces a model load that can stall (e.g. a
    # Hugging Face Hub check) and would block the MCP ``initialize`` handshake
    # past the client's startup timeout. Startup must never force a model load.
    info = check_startup(client, config)
    for w in info["warnings"]:
        print(f"[aegis-mcp] warning: {w}", file=sys.stderr)
    print(f"[aegis-mcp] namespace={config.namespace} "
          f"backend={'up' if info['reachable'] else 'down'} "
          f"embeddings={config.embedding_mode if provider.available() else 'none'}",
          file=sys.stderr)
    return MemoryTools(config, client, provider)


#: Predicates named in a tool description before it is summarised. A tool
#: description is prompt text on every request, so a registry that grew to
#: hundreds would otherwise quietly become the largest thing in the context.
VOCAB_HINT_MAX = 24


def _vocabulary(tools):
    """The typed-fact vocabulary, and whether resolving it failed.

    Returns `(vocab, ok)`. `ok` is False only when a *configured* registry
    could not be read — which is an operator error worth keeping distinct from
    "this server declares no vocabulary", because the first disables a feature
    someone asked for and the second is the ordinary default.

    Resolved regardless of `ask_pattern`, unlike before: the vocabulary is now
    also what the tool descriptions say, and a server that enforces one should
    tell the model about it whether or not the read path is turned on.
    """
    from .extract import VocabularyError, resolve_vocabulary
    try:
        return resolve_vocabulary(tools.config, tools), True
    except VocabularyError as e:
        print(f"[aegis-mcp] vocabulary: {e}", file=sys.stderr)
        return None, False


def vocabulary_hint(vocab) -> str:
    """The vocabulary, as a line to append to a tool description.

    Empty when there is none, so the description is then byte-for-byte what it
    has always been — a server without typed facts must not pay for a feature
    it does not have, in prompt tokens or in a reader's attention.

    Why it belongs in the description at all: the store can answer a question
    structurally only when the question maps onto a declared predicate, and the
    model is the thing choosing how to phrase it. Without this it is guessing
    at a contract the server enforces — the same gap the `predicates` op closed
    for programs, closed here for the model.
    """
    names = [p.name for p in (vocab or [])]
    if not names:
        return ""
    shown = sorted(names)[:VOCAB_HINT_MAX]
    more = len(names) - len(shown)
    listed = ", ".join(shown) + (f", and {more} more" if more > 0 else "")
    return (
        "\n\nThis store keeps typed facts, and a question that maps onto its "
        "vocabulary is answered from the fact graph rather than by text "
        f"similarity. The declared predicates are: {listed}. Phrasing a "
        "question in those terms — \"what does X default to?\", \"what is part "
        "of Y?\" — is what lets it be answered structurally; anything else "
        "still works and falls back to ordinary search."
    )


def _read_path(tools, vocab, vocab_ok):
    """The vocabulary and extractor the read path needs, or (None, None).

    Resolved once at startup: the vocabulary is a file the server was started
    with, or the server's own answer, and re-fetching it per query would buy
    nothing. A broken registry is said out loud and then disables the read path
    — never the whole server, which would take ordinary search down with it
    over a feature that is defined as strictly additive.

    Takes the vocabulary already resolved by `_vocabulary`, so startup asks the
    server for it once rather than once per consumer.
    """
    config = tools.config
    if not (getattr(config, "ask_pattern", False)
            or getattr(config, "ask_verbalize", False)):
        return None, None
    from .extract import make_extraction_provider
    if getattr(config, "ask_pattern", False):
        if not vocab_ok:
            return None, None  # a configured registry that could not be read
        # A missing vocabulary is not reported here: `read_path_note` says it
        # once at startup and again on every question it affects, and two
        # wordings for one condition is how a log stops being read.
    else:
        vocab = None  # verbalization needs no vocabulary
    provider = make_extraction_provider(config)
    if not provider.available():
        # Named in full, because this is the one prerequisite that is not
        # implied by the setting the operator turned on. `ask_pattern` reads
        # like a switch over the store, but expressing a question as a pattern
        # is a model call, so it runs on the *extraction* backend — and with
        # `extract_mode` at its default the feature is on in the config and
        # inert at runtime, which is indistinguishable from "the corpus had no
        # answer" in every result it returns.
        print(f"[aegis-mcp] read path: {read_path_note(config, vocab, None)}",
              file=sys.stderr)
        return None, None
    return vocab, provider


def _read_setting(config) -> str:
    """Which read-path setting is on, as a clause naming what to fix."""
    on = [name for name, flag in (("AEGIS_ASK_PATTERN", "ask_pattern"),
                                  ("AEGIS_ASK_VERBALIZE", "ask_verbalize"))
          if getattr(config, flag, False)]
    if not on:
        return "the read path is on"
    return " and ".join(on) + (" are set" if len(on) > 1 else " is set")


def read_path_note(config, vocab, extractor) -> str | None:
    """Why a configured read path cannot answer symbolically — or None.

    Attached to the search result, not only logged. `"symbolic": false` is what
    a question the corpus cannot answer and a read path that never ran look
    like alike, and stderr belongs to whoever launched the server rather than
    whoever is asking — so from the caller's side the two are indistinguishable
    and the misconfiguration reads as an empty corpus. That is exactly how it
    was reported.

    None whenever nothing is wrong, including when the feature is simply off,
    so a store that does not use it carries no extra key.
    """
    if not (getattr(config, "ask_pattern", False)
            or getattr(config, "ask_verbalize", False)):
        return None
    if extractor is None:
        mode = (getattr(config, "extract_mode", "none") or "none").lower()
        why = ("AEGIS_EXTRACT_MODE is 'none'" if mode == "none"
               else f"the {mode!r} extraction backend is unavailable "
                    "(missing CLI, SDK or API key)")
        return (f"{_read_setting(config)} but {why} — a question is "
                "turned into a pattern by the extraction backend, so set "
                "AEGIS_EXTRACT_MODE to claude-code, anthropic or openai; until "
                "then every question falls back to ordinary search and answers "
                'with "symbolic": false')
    if getattr(config, "ask_pattern", False) and not vocab:
        return ("AEGIS_ASK_PATTERN is set but no predicate vocabulary is "
                "available — neither AEGIS_EXTRACT_REGISTRY nor a registry on "
                "the server, so there is nothing to express a question against "
                "and every question falls back to ordinary search")
    return None


def search_or_ask(tools, read_vocab, read_extractor, read_note,
                  query=None, tags=None, match="any", start_time=None,
                  end_time=None, top_k=5) -> dict:
    """What `memory_search` does: the read path when it applies, else search.

    A module function rather than the body of the registered closure, so the
    routing can be tested without an MCP SDK and a live server between the
    caller and the decision.
    """
    # The read path takes the question only when the question is the whole
    # request. A pattern lookup carries no tags and no time range, so answering
    # a filtered search symbolically would drop the filters and return
    # something that looks like an answer to what was asked.
    unfiltered = bool(query) and not tags and start_time is None \
        and end_time is None
    if unfiltered and read_extractor is not None:
        res = ask(tools, query, read_vocab, tools.config, read_extractor,
                  top_k=top_k)
        res = verbalize_all(tools, res, tools.config, read_extractor)
    else:
        res = tools.search(query=query, tags=tags, match=match,
                           start_time=start_time, end_time=end_time,
                           top_k=top_k, lexical=True)
    # Only ever present when the read path is configured and cannot run, and
    # only on a question it would have taken — so a correctly wired store and a
    # filtered search are byte-identical to what they were.
    if unfiltered and read_note and isinstance(res, dict):
        res.setdefault("read_path", read_note)
    return res


def main() -> int:
    # Ergonomic alias: `uvx aegisdb-mcp init …` runs the setup scaffolder. Claude
    # Code launches the server with no args, so this never shadows normal use.
    if len(sys.argv) > 1 and sys.argv[1] == "init":
        from .init import main as init_main
        return init_main(sys.argv[2:])

    server_cls = _server_class()
    if server_cls is None:
        print(_sdk_error(), file=sys.stderr)
        return 1

    try:
        tools = build_tools()
    except ConfigError as e:
        # A traceback here is the wrong shape of message: the operator wrote a
        # path or a value, and what they need is that sentence, not a stack.
        print(f"[aegis-mcp] config: {e}", file=sys.stderr)
        return 1
    mcp = _new_server(server_cls, "memory")
    vocab, vocab_ok = _vocabulary(tools)
    read_vocab, read_extractor = _read_path(tools, vocab, vocab_ok)
    hint = vocabulary_hint(vocab)
    if hint:
        print(f"[aegis-mcp] vocabulary: {len(vocab)} predicate(s) named in the "
              f"memory_search description", file=sys.stderr)
    read_note = read_path_note(tools.config, read_vocab, read_extractor)
    if read_note:
        # `_read_path` already printed the no-backend case on its way out; this
        # covers the rest, so every reason a configured read path sits inert is
        # said at startup as well as on each question that hits it.
        if read_extractor is not None:
            print(f"[aegis-mcp] read path: {read_note}", file=sys.stderr)
    elif read_extractor is not None:
        # Said out loud when it is *on*, not only when it fails. Until now the
        # log told a working read path from a silently disabled one by the
        # absence of a complaint, which is no way to answer "why is `symbolic`
        # always false?" — the question this line exists to close.
        print(f"[aegis-mcp] read path: on via {tools.config.extract_mode}"
              f" ({len(read_vocab) if read_vocab else 0} predicate(s))",
              file=sys.stderr)

    @mcp.tool()
    def memory_save(text: str, tags: list[str] | None = None,
                    importance: float = 0.5, semantic: bool = False,
                    confidence: float = 1.0) -> dict:
        """Persist a memory so it can be recalled in future sessions."""
        return tools.save(text, tags=tags, importance=importance,
                          semantic=semantic, confidence=confidence)

    # Registered explicitly rather than with @mcp.tool(), because the
    # vocabulary is only known at runtime and a docstring literal cannot carry
    # it. Both SDK majors read `__doc__` at registration, so setting it first
    # and registering after is the one mechanism that works on each.
    def memory_search(query: str | None = None, tags: list[str] | None = None,
                      match: str = "any", start_time: int | None = None,
                      end_time: int | None = None, top_k: int = 5) -> dict:
        """Recall relevant memories by meaning, exact keyword, tags, and/or
        recency. `query` matches both semantically and literally, so searching an
        exact identifier — a flag like `--tenant-max-records`, a `file.c:line`
        reference, an error code — finds the memory containing that token."""
        return search_or_ask(tools, read_vocab, read_extractor, read_note,
                             query=query, tags=tags, match=match,
                             start_time=start_time, end_time=end_time,
                             top_k=top_k)

    memory_search.__doc__ = (memory_search.__doc__ or "") + hint
    mcp.tool()(memory_search)

    @mcp.tool()
    def memory_get(id: int) -> dict:
        """Retrieve a specific memory by its id."""
        return tools.get(id)

    @mcp.tool()
    def memory_update(id: int, text: str | None = None,
                      confidence: float | None = None,
                      tags: list[str] | None = None) -> dict:
        """Revise a semantic memory (episodic memories are immutable)."""
        return tools.update(id, text=text, confidence=confidence, tags=tags)

    @mcp.tool()
    def memory_relate(from_id: int, to_id: int, kind: str | None = None) -> dict:
        """Link two memories with a directed relationship."""
        return tools.relate(from_id, to_id, kind=kind)

    mcp.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())