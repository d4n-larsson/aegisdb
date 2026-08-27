"""How this package runs the Claude Code CLI as a subprocess, in one place.

Two backends shell out to `claude -p` — extraction (`extract_mode: claude-code`)
and distillation (`summary_mode: claude-code`) — and both do it from inside a
capture, which is the one context where a Claude Code session is dangerous to
start. The session ends the moment the answer is printed; its end fires the
SessionEnd hook; that hook captures; that capture asks a model again. With the
capture worker detached from the hook's process group (see ``hooks._detach``)
nothing reaps the tree, and each round multiplies rather than terminating.

So a completion is not a bare `subprocess.run(["claude", "-p", prompt])` — it is
that command plus two independent stops, and this module exists so neither
backend can be written without them:

- ``child_env()`` marks the environment the child inherits, and the hook returns
  immediately when it sees the mark.
- ``headless_cmd()`` adds ``--safe-mode``, so the child loads no hooks at all.

Either stop ends the recursion alone. Both are here because the failure mode is
a machine out of processes, and the cost of the second is a probed flag.
"""
from __future__ import annotations

import os
import subprocess

from .hooks import CAPTURE_ACTIVE_ENV

#: Disables hooks — and CLAUDE.md, MCP servers, skills, none of which a one-shot
#: prompt wants — while leaving auth, model selection and permissions normal.
SAFE_MODE = "--safe-mode"

_safe_mode_supported: bool | None = None


def supports_safe_mode() -> bool:
    """Whether the `claude` on PATH takes ``--safe-mode``. Probed once, cached.

    Probed rather than assumed because a CLI predating the flag rejects the whole
    invocation: passing it blind would fail every completion and silently drop
    capture to its heuristic path. Unknown reads as unsupported — the environment
    mark still stops the recursion on its own.

    Both streams are searched and a clean exit is required, so that a wrapper
    printing help on stderr is still read correctly and a `--help` that errors is
    not mined for a flag it never listed. The timeout is short because it is not
    counted against the caller's own budget: the first completion of a worker
    spends this before extraction's 120s (or distillation's 90s) even starts, and
    the cache is per-process, so every detached capture pays it once.
    """
    global _safe_mode_supported
    if _safe_mode_supported is None:
        try:
            r = subprocess.run(["claude", "--help"], capture_output=True,
                               text=True, timeout=10)
            help_text = (r.stdout or "") + (r.stderr or "")
            _safe_mode_supported = r.returncode == 0 and SAFE_MODE in help_text
        except (subprocess.TimeoutExpired, OSError):
            _safe_mode_supported = False
    return _safe_mode_supported


def headless_cmd(prompt: str, model: str = "") -> list[str]:
    """The argv for one headless completion."""
    cmd = ["claude", "-p", prompt]
    if model:
        cmd += ["--model", model]
    if supports_safe_mode():
        cmd.append(SAFE_MODE)
    return cmd


def child_env() -> dict[str, str]:
    """This process's environment, marked as a capture already in progress.

    Marked here rather than trusted from the caller: these backends are also
    reached from the MCP server and the CLI, where no hook has claimed anything.
    """
    env = dict(os.environ)
    env[CAPTURE_ACTIVE_ENV] = "1"
    return env
