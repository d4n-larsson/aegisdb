#!/usr/bin/env python3
"""UserPromptSubmit hook shim (US2 / T025).

Delegates to ``aegis_mcp.hooks.recall``. Kept as a path-runnable script for
checkouts; the packaged console script ``aegis-recall-hook`` (and
``uvx --from aegis-mcp aegis-recall-hook``) run the same code without a clone.
"""
import os
import sys

# Make the sibling aegis_mcp package importable when run as a script.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from aegis_mcp.hooks import recall  # noqa: E402

if __name__ == "__main__":
    sys.exit(recall())