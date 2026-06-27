#!/usr/bin/env python3
"""SessionEnd (opt-in Stop) hook: persist salient memories (US3 / T031).

Reads the hook event JSON on stdin and stores salient outcomes of the ended
session. Emits nothing on stdout. ALWAYS exits 0 — capture failures must not
disrupt session teardown (FR-009).
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main() -> int:
    try:
        event = json.loads(sys.stdin.read() or "{}")
    except ValueError:
        return 0

    try:
        from aegis_mcp.config import load_config
        from aegis_mcp.embeddings import make_provider
        from aegis_mcp.capture import run_capture

        cwd = event.get("cwd")
        config = load_config(cwd=cwd)
        if not config.capture_enabled:
            return 0

        # Honour capture scope: only run the per-turn Stop event if configured.
        scope = config.capture_scope
        ev = event.get("hook_event_name", "")
        if ev == "Stop" and scope != "turn":
            return 0

        provider = make_provider(config)
        stored = run_capture(event, config, provider)
        if stored:
            print(f"[aegis-mcp capture] stored {stored} mem(s)", file=sys.stderr)
    except Exception as exc:
        print(f"[aegis-mcp capture] {exc}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())