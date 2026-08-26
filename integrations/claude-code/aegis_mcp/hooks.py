"""Hook entry points (US2/US3), packaged so they can run via a console script
(`uvx --from aegis-mcp aegis-recall-hook`) as well as by file path.

Both read the Claude Code hook event JSON on stdin and ALWAYS exit 0 — memory is
best-effort and must never block or fail a turn (FR-005/FR-009). The thin scripts
under ``hooks/`` delegate here so path-based and uvx-based wiring share one code
path.

Recall answers inline — its whole output is the context it injects. Capture does
not: it detaches into its own session and lets the hook return at once, because
SessionEnd runs during shutdown and cancels a hook that is still working (see
``_detach``).
"""
from __future__ import annotations

import json
import os
import sys


def _detach_enabled() -> bool:
    return os.environ.get("AEGIS_CAPTURE_DETACH", "1").strip().lower() not in (
        "0", "false", "no", "off")


def _quiet_stdio() -> None:
    """Point the worker's stdio somewhere that outlives the hook.

    Its stdout/stderr are pipes Claude Code stops reading the moment the hook
    returns, so keeping them would risk writing into a closed pipe for the rest
    of the capture. AEGIS_CAPTURE_LOG keeps the diagnostics that would otherwise
    go to the hook's stderr — the `stored N mem(s)` line, the triple counts, the
    reasons a feature stayed inert — which are the only view into a run that now
    happens after Claude Code has exited.
    """
    devnull = os.open(os.devnull, os.O_RDWR)
    out = devnull
    log = os.environ.get("AEGIS_CAPTURE_LOG", "").strip()
    if log:
        try:
            parent = os.path.dirname(log)
            if parent:
                os.makedirs(parent, exist_ok=True)
            out = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        except OSError:
            out = devnull  # an unwritable log must not cost us the capture
    os.dup2(devnull, 0)
    os.dup2(out, 1)
    os.dup2(out, 2)


def _detach() -> bool:
    """Move the capture into a session of its own. True = caller must return NOW.

    SessionEnd fires while Claude Code is shutting down, and a hook that has not
    finished by then is CANCELLED — measured at ~1.5s (Claude Code 2.1.243),
    reported as `SessionEnd hook [...] failed: Hook cancelled`. Capture cannot
    win that race and never could: `embedding_mode: local` reloads the sentence
    model in a fresh process (~4.6s) and `extract_mode` shells out to a model
    (~38s for a full transcript). Wired straight in, it wrote nothing, silently,
    for every session — the hook's own "never fail a turn" contract is what made
    it silent.

    `setsid` is the fix: the cancellation kills the hook's process GROUP, so a
    worker that has left that group survives it, and the parent exiting at once
    means Claude Code has nothing left to cancel. Returns True in the parent
    (which exits 0 immediately), False in the worker — and False in the parent
    when detaching is off or unavailable, which runs the capture inline exactly
    as before.
    """
    if not _detach_enabled() or not hasattr(os, "fork"):
        return False
    try:
        pid = os.fork()
    except OSError as exc:  # inline is worse than detached, better than nothing
        print(f"[aegis-mcp capture] detach failed ({exc}); running inline",
              file=sys.stderr)
        return False
    if pid > 0:
        # Said on the parent's stderr, which Claude Code still reads: a capture
        # that now finishes after the session is gone would otherwise leave no
        # sign it ever started.
        print(f"[aegis-mcp capture] detached (pid {pid})", file=sys.stderr)
        return True
    try:
        os.setsid()
    except OSError:
        pass  # already a session leader, or refused: the fork alone still helps
    _quiet_stdio()
    return False


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
    """SessionEnd (opt-in Stop) hook: persist salient memories.

    Returns as soon as there is a worker to do the storing (see ``_detach``), so
    the exit code says the capture STARTED, never that it finished. Set
    ``AEGIS_CAPTURE_DETACH=0`` to run it inline — what tests want, and what a
    caller wanting the exit code to mean something wants.
    """
    try:
        event = json.loads(sys.stdin.read() or "{}")
    except ValueError:
        return 0

    try:
        from .config import load_config

        cwd = event.get("cwd")
        config = load_config(cwd=cwd)
        if not config.capture_enabled:
            return 0

        # Honour capture scope: only run the per-turn Stop event if configured.
        if event.get("hook_event_name", "") == "Stop" and config.capture_scope != "turn":
            return 0

        # Everything above is cheap and decides whether there is work at all, so
        # it stays in the hook's own process: a turn that captures nothing should
        # not fork, and should not announce a worker it does not need.
        if _detach():
            return 0

        from .embeddings import make_provider
        from .capture import run_capture

        provider = make_provider(config)
        stored = run_capture(event, config, provider)
        if stored:
            print(f"[aegis-mcp capture] stored {stored} mem(s)", file=sys.stderr)
    except Exception as exc:
        print(f"[aegis-mcp capture] {exc}", file=sys.stderr)
    return 0

def main(argv: list[str] | None = None) -> int:
    """`python -m aegis_mcp.hooks recall|capture`.

    A second way to reach the same two functions, and the reason it exists is
    launcher independence: the console scripts (`aegisdb-recall-hook`) go
    through whatever `uvx`/`pip` installed as an executable, and a launcher that
    cannot exec that shim takes both hooks down while `python -m` on the same
    package keeps working. A hook wired this way survives that, and
    `aegisdb-init` falls back to it when it finds the shim does not run.
    """
    argv = sys.argv[1:] if argv is None else argv
    which = (argv[0] if argv else "").strip().lower()
    if which == "recall":
        return recall()
    if which == "capture":
        return capture()
    print("usage: python -m aegis_mcp.hooks recall|capture", file=sys.stderr)
    return 2


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
