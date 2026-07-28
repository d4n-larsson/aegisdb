"""MCP server binding (US1 surface / T014).

Thin wrapper: it lazy-imports the optional ``mcp`` SDK and registers tools that
delegate straight to :class:`aegis_mcp.tools.MemoryTools`. All behaviour lives in
the core modules, so this file is just protocol glue — it is the only place that
requires ``mcp`` to be installed.

Both SDK majors are supported; see :func:`_server_class`.
"""
from __future__ import annotations

import sys

from .client import AegisClient, check_startup
from .config import load_config
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

    tools = build_tools()
    mcp = _new_server(server_cls, "memory")

    @mcp.tool()
    def memory_save(text: str, tags: list[str] | None = None,
                    importance: float = 0.5, semantic: bool = False,
                    confidence: float = 1.0) -> dict:
        """Persist a memory so it can be recalled in future sessions."""
        return tools.save(text, tags=tags, importance=importance,
                          semantic=semantic, confidence=confidence)

    @mcp.tool()
    def memory_search(query: str | None = None, tags: list[str] | None = None,
                      match: str = "any", start_time: int | None = None,
                      end_time: int | None = None, top_k: int = 5) -> dict:
        """Recall relevant memories by meaning, tags, and/or recency."""
        return tools.search(query=query, tags=tags, match=match,
                           start_time=start_time, end_time=end_time, top_k=top_k)

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