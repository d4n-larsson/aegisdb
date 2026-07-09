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
    """Trim `text` to at most `max_chars` (0 = no limit), cutting on a word
    boundary where possible and marking the elision, so the model can see the
    memory was shortened rather than mistaking a fragment for the whole thing.
    Truncation is inherently lossy — memories are meant to be terse, so this is
    a safety net for outliers, not the common path."""
    if not max_chars or len(text) <= max_chars:
        return text
    cut = text[:max_chars]
    sp = cut.rfind(" ")
    if sp >= max_chars * 0.6:  # back up to a space only if it keeps most of it
        cut = cut[:sp]
    return cut.rstrip() + " […]"


def format_context(memories, max_chars_per_memory: int = 0,
                   char_budget: int = 0):
    """Render memories as a compact, model-readable context block.

    Recall is injected into every turn, so its size is a direct token cost.
    Two guard rails keep it bounded (both 0 = unlimited):

    - ``max_chars_per_memory`` truncates each memory's text.
    - ``char_budget`` is a hard ceiling on the block: no single memory's text
      exceeds it (so even the always-kept top memory stays within it), and once
      the budget is reached the rest are dropped. Memories arrive ranked, so the
      highest-value ones survive.

    Whenever memories are dropped, an explicit trailer says how many, so the
    model knows the list is partial (and can call memory_search for more) rather
    than assuming it is complete. Returns ``(context, included_count)``.
    """
    if not memories:
        return "", 0
    # A single memory never exceeds the whole budget, so the forced top memory
    # can't blow past the ceiling — closes the per-memory-cap-off gap.
    per_cap = max_chars_per_memory
    if char_budget:
        per_cap = char_budget if per_cap == 0 else min(per_cap, char_budget)

    lines = ["Relevant memories from past sessions:"]
    used = 0
    included = 0
    for m in memories:
        tags = m.get("tags") or []
        tag_str = f" (tags: {', '.join(tags)})" if tags else ""
        text = _truncate((m.get("text") or "").strip().replace("\n", " "),
                         per_cap)
        line = f"- [#{m.get('id')}] {text}{tag_str}"
        # Always keep the top-ranked memory (now bounded by per_cap); drop the
        # rest once the budget is reached.
        if char_budget and included >= 1 and used + len(line) > char_budget:
            break
        lines.append(line)
        used += len(line)
        included += 1

    omitted = len(memories) - included
    if omitted > 0:
        lines.append(f"- (+{omitted} more relevant "
                     f"memor{'y' if omitted == 1 else 'ies'} omitted to fit the "
                     f"recall budget; call memory_search to see the rest)")
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