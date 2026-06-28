#!/usr/bin/env python3
"""SessionEnd (opt-in Stop) hook shim (US3 / T031).

Delegates to ``aegis_mcp.hooks.capture``. Kept as a path-runnable script for
checkouts; the packaged console script ``aegisdb-capture-hook`` (and
``uvx --from aegisdb-mcp aegisdb-capture-hook``) run the same code without a clone.
"""
import os
import sys

# Make the sibling aegis_mcp package importable when run as a script.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from aegis_mcp.hooks import capture  # noqa: E402

if __name__ == "__main__":
    sys.exit(capture())