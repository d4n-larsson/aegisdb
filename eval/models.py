"""Answer-model seam for the A/B task benchmark (eval/ab_tasks.py).

The benchmark measures whether an agent answers multi-session tasks better *with*
AegisDB memory than *without* it, so it needs something that answers a prompt.
Same shape as the embedding/summary seams: one interface selected by flag, any
SDK imported lazily. Backends:

  - `fake`        : deterministic, dependency-free (CI). Answers from the prompt's
                    Context block, or admits ignorance when there is none — so the
                    harness demonstrably measures the memory effect without a model.
  - `claude-code` : the `claude` CLI in headless mode (reuses local auth).
  - `anthropic`   : Anthropic Messages API (`anthropic` SDK + ANTHROPIC_API_KEY).
  - `openai`      : OpenAI-compatible chat API (`openai` SDK + OPENAI_API_KEY).
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess

_ANTHROPIC_DEFAULT_MODEL = "claude-haiku-4-5-20251001"
_OPENAI_DEFAULT_MODEL = "gpt-4o-mini"
_MAX_TOKENS = 512


def _looks_like_key(v) -> bool:
    return bool(v) and any(c.isalnum() for c in v.strip())


class AnswerModel:
    def available(self) -> bool:
        return False

    def answer(self, prompt: str) -> str:
        raise NotImplementedError


class FakeAnswerModel(AnswerModel):
    """Answers from the Context block if the prompt has one, else says it doesn't
    know. This makes the ON arm (context injected) succeed and the OFF arm (no
    context) fail on a well-formed task — proving the harness isolates the memory
    effect. A real backend replaces the regurgitation with genuine reasoning."""

    _CTX = re.compile(r"Context:\s*(.*?)\n\s*Question:", re.DOTALL)

    def available(self) -> bool:
        return True

    def answer(self, prompt: str) -> str:
        m = self._CTX.search(prompt)
        ctx = m.group(1).strip() if m else ""
        return ctx if ctx else "I don't have that information."


class _CLIAnswerModel(AnswerModel):
    def _complete(self, prompt: str) -> str | None:
        raise NotImplementedError

    def answer(self, prompt: str) -> str:
        out = self._complete(prompt)
        return (out or "").strip()


class ClaudeCodeAnswerModel(_CLIAnswerModel):
    def __init__(self, model="", timeout_s=120.0):
        self._model, self._timeout = model or "", timeout_s

    def available(self) -> bool:
        return shutil.which("claude") is not None

    def _complete(self, prompt: str) -> str | None:
        cmd = ["claude", "-p", prompt] + (["--model", self._model] if self._model else [])
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=self._timeout)
        except (subprocess.TimeoutExpired, OSError):
            return None
        return r.stdout if r.returncode == 0 else None


class AnthropicAnswerModel(_CLIAnswerModel):
    def __init__(self, model="", timeout_s=120.0):
        self._model, self._timeout, self._client = model or _ANTHROPIC_DEFAULT_MODEL, timeout_s, None

    def available(self) -> bool:
        try:
            import anthropic  # noqa: F401
        except ImportError:
            return False
        return _looks_like_key(os.environ.get("ANTHROPIC_API_KEY"))

    def _complete(self, prompt: str) -> str | None:
        try:
            if self._client is None:
                import anthropic
                self._client = anthropic.Anthropic()
            r = self._client.messages.create(
                model=self._model, max_tokens=_MAX_TOKENS, timeout=self._timeout,
                messages=[{"role": "user", "content": prompt}])
        except Exception:
            return None
        return "".join(getattr(b, "text", "") for b in (r.content or [])
                       if getattr(b, "type", "") == "text")


class OpenAIAnswerModel(_CLIAnswerModel):
    def __init__(self, model="", api_base="", timeout_s=120.0):
        self._model = model or _OPENAI_DEFAULT_MODEL
        self._api_base, self._timeout, self._client = api_base or "", timeout_s, None

    def available(self) -> bool:
        try:
            import openai  # noqa: F401
        except ImportError:
            return False
        return _looks_like_key(os.environ.get("OPENAI_API_KEY"))

    def _complete(self, prompt: str) -> str | None:
        try:
            if self._client is None:
                import openai
                kw = {"timeout": self._timeout}
                if self._api_base:
                    kw["base_url"] = self._api_base
                self._client = openai.OpenAI(**kw)
            r = self._client.chat.completions.create(
                model=self._model, max_tokens=_MAX_TOKENS,
                messages=[{"role": "user", "content": prompt}])
        except Exception:
            return None
        return (r.choices[0].message.content or "") if r.choices else ""


def resolve_model(name: str, model_name="", api_base="") -> AnswerModel:
    if name == "fake":
        return FakeAnswerModel()
    if name == "claude-code":
        return ClaudeCodeAnswerModel(model_name)
    if name == "anthropic":
        return AnthropicAnswerModel(model_name)
    if name == "openai":
        return OpenAIAnswerModel(model_name, api_base)
    raise ValueError(f"unknown model backend: {name}")