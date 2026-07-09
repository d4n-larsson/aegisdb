"""Automatic recall logic (US2 / T022–T024).

Builds a search from the user's prompt, ranks and filters hits, and formats a
compact context block. The whole pass is bounded by ``recall_time_budget_ms``:
the AegisDB read timeout is set to the budget, and an overall deadline guards
against the pass exceeding it, so recall never blocks the turn (FR-005, SC-002).
On any failure or timeout it returns an empty, ``degraded`` result.
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field

from .client import AegisClient
from .embeddings import EmbeddingProvider
from .tools import MemoryTools


@dataclass
class RecallResult:
    memories: list = field(default_factory=list)
    context: str = ""
    degraded: bool = False
    truncated: bool = False
    elapsed_ms: int = 0


def _truncate(text: str, max_chars: int) -> str:
    """Trim `text` to `max_chars` (0 = no limit), marking elision with an ellipsis."""
    if max_chars and len(text) > max_chars:
        return text[:max_chars].rstrip() + "…"
    return text


def format_context(memories, max_chars_per_memory: int = 0,
                   char_budget: int = 0):
    """Render memories as a compact, model-readable context block.

    Recall is injected into every turn, so its size is a direct token cost.
    Each memory's text is capped at ``max_chars_per_memory`` and the block stops
    once its rendered bodies reach ``char_budget`` (both 0 = unlimited). Memories
    arrive already ranked, so the budget keeps the highest-value ones and drops
    the tail. Returns ``(context, included_count)``; the top memory is always
    included (already length-capped) even if it alone exceeds the budget.
    """
    if not memories:
        return "", 0
    lines = ["Relevant memories from past sessions:"]
    used = 0
    included = 0
    for m in memories:
        tags = m.get("tags") or []
        tag_str = f" (tags: {', '.join(tags)})" if tags else ""
        text = _truncate((m.get("text") or "").strip().replace("\n", " "),
                         max_chars_per_memory)
        line = f"- [#{m.get('id')}] {text}{tag_str}"
        if char_budget and included >= 1 and used + len(line) > char_budget:
            break  # keep the highest-ranked slice within budget; drop the rest
        lines.append(line)
        used += len(line)
        included += 1
    return "\n".join(lines), included


def run_recall(prompt: str, config, provider: EmbeddingProvider,
               client: AegisClient | None = None, clock=time.monotonic) -> RecallResult:
    start = clock()
    budget_ms = config.recall_time_budget_ms
    if client is None:
        # Bound the backend read by the recall budget so a slow/hung backend
        # cannot stall the turn.
        client = AegisClient.from_config(
            config,
            connect_timeout_ms=min(config.connect_timeout_ms, budget_ms),
            read_timeout_ms=budget_ms)

    if not prompt or not prompt.strip():
        return RecallResult(degraded=False, elapsed_ms=0)

    tools = MemoryTools(config, client, provider)
    res = tools.search(query=prompt, top_k=config.recall_top_k)
    elapsed_ms = int((clock() - start) * 1000)

    # Over budget (even if the call returned) -> treat as degraded, inject nothing.
    if elapsed_ms > budget_ms and not res.get("ok"):
        return RecallResult(degraded=True, elapsed_ms=elapsed_ms)
    if not res.get("ok"):
        return RecallResult(degraded=True, elapsed_ms=elapsed_ms)

    memories = res.get("memories", [])
    context, included = format_context(
        memories,
        max_chars_per_memory=config.recall_max_chars_per_memory,
        char_budget=config.recall_char_budget)
    # Truncated if the backend capped at top_k, or the char budget dropped some
    # of the returned memories from the injected block.
    truncated = (res.get("total", len(memories)) >= config.recall_top_k
                 or included < len(memories))
    return RecallResult(
        memories=memories,
        context=context,
        degraded=bool(res.get("degraded")),
        truncated=truncated,
        elapsed_ms=elapsed_ms,
    )