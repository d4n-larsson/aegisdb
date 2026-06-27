#!/usr/bin/env python3
"""UserPromptSubmit hook: inject relevant memories into context (US2 / T025).

Reads the hook event JSON on stdin, runs best-effort recall, and emits a
``hookSpecificOutput.additionalContext`` block on stdout. ALWAYS exits 0 — memory
is best-effort and must never block or fail the turn (FR-005/FR-009).
"""
import json
import os
import sys

# Make the sibling aegis_mcp package importable when run as a script.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main() -> int:
    try:
        event = json.loads(sys.stdin.read() or "{}")
    except ValueError:
        return 0  # malformed event: proceed with no injected memory

    try:
        from aegis_mcp.config import load_config
        from aegis_mcp.embeddings import make_provider
        from aegis_mcp.recall import run_recall

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


if __name__ == "__main__":
    sys.exit(main())