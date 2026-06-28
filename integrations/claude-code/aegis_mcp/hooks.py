"""Hook entry points (US2/US3), packaged so they can run via a console script
(`uvx --from aegis-mcp aegis-recall-hook`) as well as by file path.

Both read the Claude Code hook event JSON on stdin and ALWAYS exit 0 — memory is
best-effort and must never block or fail a turn (FR-005/FR-009). The thin scripts
under ``hooks/`` delegate here so path-based and uvx-based wiring share one code
path.
"""
from __future__ import annotations

import json
import sys


def recall() -> int:
    """UserPromptSubmit hook: inject relevant memories into context."""
    try:
        event = json.loads(sys.stdin.read() or "{}")
    except ValueError:
        return 0  # malformed event: proceed with no injected memory

    try:
        from .config import load_config
        from .embeddings import make_provider
        from .recall import run_recall

        prompt = event.get("prompt", "")
        cwd = event.get("cwd")
        config = load_config(cwd=cwd)
        if not config.recall_enabled or not prompt.strip():
            return 0

        provider = make_provider(config)
        result = run_recall(prompt, config, provider)
        if result.context:
            out = {
                "hookSpecificOutput": {
                    "hookEventName": "UserPromptSubmit",
                    "additionalContext": result.context,
                }
            }
            sys.stdout.write(json.dumps(out))
    except Exception as exc:  # never surface as a turn failure
        print(f"[aegis-mcp recall] {exc}", file=sys.stderr)
    return 0


def capture() -> int:
    """SessionEnd (opt-in Stop) hook: persist salient memories."""
    try:
        event = json.loads(sys.stdin.read() or "{}")
    except ValueError:
        return 0

    try:
        from .config import load_config
        from .embeddings import make_provider
        from .capture import run_capture

        cwd = event.get("cwd")
        config = load_config(cwd=cwd)
        if not config.capture_enabled:
            return 0

        # Honour capture scope: only run the per-turn Stop event if configured.
        if event.get("hook_event_name", "") == "Stop" and config.capture_scope != "turn":
            return 0

        provider = make_provider(config)
        stored = run_capture(event, config, provider)
        if stored:
            print(f"[aegis-mcp capture] stored {stored} mem(s)", file=sys.stderr)
    except Exception as exc:
        print(f"[aegis-mcp capture] {exc}", file=sys.stderr)
    return 0