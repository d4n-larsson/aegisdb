"""FastMCP server binding (US1 surface / T014).

Thin wrapper: it lazy-imports the optional ``mcp`` SDK and registers tools that
delegate straight to :class:`aegis_mcp.tools.MemoryTools`. All behaviour lives in
the core modules, so this file is just protocol glue — it is the only place that
requires ``mcp`` to be installed.
"""
from __future__ import annotations

import sys

from .client import AegisClient, check_startup
from .config import load_config
from .embeddings import make_provider
from .tools import MemoryTools


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
    try:
        from mcp.server.fastmcp import FastMCP
    except ImportError:
        print("[aegis-mcp] the 'mcp' package is required to run the server: "
              "pip install mcp", file=sys.stderr)
        return 1

    tools = build_tools()
    mcp = FastMCP("memory")

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