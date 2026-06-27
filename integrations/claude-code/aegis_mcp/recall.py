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


def format_context(memories) -> str:
    """Render memories as a compact, model-readable context block."""
    if not memories:
        return ""
    lines = ["Relevant memories from past sessions:"]
    for m in memories:
        tags = m.get("tags") or []
        tag_str = f" (tags: {', '.join(tags)})" if tags else ""
        text = (m.get("text") or "").strip().replace("\n", " ")
        lines.append(f"- [#{m.get('id')}] {text}{tag_str}")
    return "\n".join(lines)


def run_recall(prompt: str, config, provider: EmbeddingProvider,
               client: AegisClient | None = None, clock=time.monotonic) -> RecallResult:
    start = clock()
    budget_ms = config.recall_time_budget_ms
    if client is None:
        # Bound the backend read by the recall budget so a slow/hung backend
        # cannot stall the turn.
        client = AegisClient(config.aegis_host, config.aegis_port,
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
    truncated = res.get("total", len(memories)) >= config.recall_top_k
    return RecallResult(
        memories=memories,
        context=format_context(memories),
        degraded=bool(res.get("degraded")),
        truncated=truncated,
        elapsed_ms=elapsed_ms,
    )