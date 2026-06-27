"""AegisDB ↔ Claude Code memory integration.

This package provides the integration layer that makes AegisDB the persistent
long-term memory of Claude Code:

- A FastMCP server (``aegis_mcp.server``) exposing agent-callable memory tools.
- Pure, dependency-free core modules (client/config/embeddings/results/tools/
  recall/capture) that hold all logic and are importable without the ``mcp`` SDK.
- Hook entry points (``hooks/``) for automatic recall and capture.

Only ``server`` imports the optional ``mcp`` dependency; everything else runs on
the Python standard library so the logic stays testable in any environment.
"""

__version__ = "0.1.0"