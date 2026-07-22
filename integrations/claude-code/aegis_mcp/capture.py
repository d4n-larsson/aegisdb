"""Automatic capture logic (US3 / T028–T030).

Derives salient memories from a finished session, filters out ephemeral and
low-value items, and persists the survivors as episodic memories. When nothing
is salient, nothing is written (FR-007). Content the user marked ephemeral is
excluded (FR-014).
"""
from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass, field

from .client import AegisClient
from .embeddings import EmbeddingProvider
from .extract import make_extraction_provider
from .tools import MemoryTools

# Phrases that signal a salient outcome worth remembering, with a weight.
_SALIENCE_MARKERS = {
    "remember": 1.0, "decided": 0.8, "decision": 0.8, "we'll use": 0.7,
    "we will use": 0.7, "prefer": 0.7, "convention": 0.7, "always": 0.6,
    "never": 0.6, "the fix": 0.7, "fixed": 0.6, "root cause": 0.7,
    "important": 0.6, "note that": 0.5, "going forward": 0.7,
}
# Phrases that mark content as not-for-long-term-memory.
_EPHEMERAL_MARKERS = (
    "don't remember", "do not remember", "ephemeral", "temporary",
    "scratch", "ignore this", "forget this", "secret", "do not store",
)
# Tag inference from content.
_TAG_RULES = {
    "decision": ("decided", "decision", "we'll use", "we will use", "going forward"),
    "preference": ("prefer", "convention", "always", "never"),
    "fix": ("the fix", "fixed", "root cause", "bug"),
}


@dataclass
class CaptureCandidate:
    text: str
    salience: float
    tags: list = field(default_factory=list)
    ephemeral: bool = False


def _infer_tags(low: str) -> list:
    tags = ["session"]
    for tag, phrases in _TAG_RULES.items():
        if any(p in low for p in phrases):
            tags.append(tag)
    return tags


def score_text(text: str) -> CaptureCandidate:
    low = text.lower()
    ephemeral = any(m in low for m in _EPHEMERAL_MARKERS)
    salience = 0.0
    for marker, weight in _SALIENCE_MARKERS.items():
        if marker in low:
            salience = max(salience, weight)
    # Longer, substantive statements get a small boost; trivial ones capped.
    if salience > 0 and len(text.split()) >= 6:
        salience = min(1.0, salience + 0.1)
    return CaptureCandidate(text=text.strip(), salience=salience,
                            tags=_infer_tags(low), ephemeral=ephemeral)


def _iter_texts(obj):
    """Best-effort extraction of human-readable text from a transcript entry."""
    if isinstance(obj, str):
        yield obj
    elif isinstance(obj, dict):
        if isinstance(obj.get("text"), str):
            yield obj["text"]
        content = obj.get("content")
        if isinstance(content, str):
            yield content
        elif isinstance(content, (list, dict)):
            yield from _iter_texts(content)
        msg = obj.get("message")
        if isinstance(msg, (dict, list)):
            yield from _iter_texts(msg)
    elif isinstance(obj, list):
        for item in obj:
            yield from _iter_texts(item)


def load_transcript(path: str | None) -> list:
    """Read a JSONL transcript into a list of text snippets. Robust to missing
    files and varied entry shapes; returns [] on any problem."""
    if not path or not os.path.isfile(path):
        return []
    texts = []
    try:
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except ValueError:
                    texts.append(line)
                    continue
                texts.extend(t for t in _iter_texts(obj) if t and t.strip())
    except OSError:
        return []
    return texts


# Split a snippet into sentence-ish units so one marker doesn't drag in a whole
# paragraph.
_SENT = re.compile(r"[^.!?\n]+[.!?]?")


def derive_candidates(texts, config) -> list:
    candidates = []
    seen = set()
    for text in texts:
        for sent in _SENT.findall(text):
            sent = sent.strip()
            if len(sent) < 8:
                continue
            cand = score_text(sent)
            if cand.salience <= 0:
                continue
            key = cand.text.lower()
            if key in seen:
                continue
            seen.add(key)
            candidates.append(cand)
    return candidates


def filter_candidates(candidates, config) -> list:
    return [c for c in candidates
            if not c.ephemeral and c.salience >= config.capture_min_salience]


def _drop_ephemeral(texts: list) -> list:
    """Remove snippets the user marked not-for-long-term-memory (FR-014) before
    they reach the extractor, so the ephemeral guarantee holds regardless of the
    model's behavior."""
    return [t for t in texts
            if not any(m in t.lower() for m in _EPHEMERAL_MARKERS)]


def extract_facts(texts: list, config, provider=None) -> list | None:
    """LLM extraction path (ROADMAP 2.1): distil the transcript into durable
    Facts. Returns a list[Fact] (possibly empty) when an extractor ran, or None
    when extraction is disabled/unavailable/failed so the caller falls back to
    heuristic capture. Never raises."""
    if provider is None:
        provider = make_extraction_provider(config)
    if not provider.available():
        return None
    kept = _drop_ephemeral(texts)
    if not kept:
        return []
    blob = "\n".join(kept)
    cap = getattr(config, "extract_max_input_chars", 24000)
    if cap and len(blob) > cap:
        blob = blob[-cap:]  # keep the most recent turns
    try:
        return provider.extract(blob, getattr(config, "extract_max_facts", 12))
    except Exception:
        return None  # never let extraction break capture


def _find_superseded_ids(fact, tools, config, extractor) -> list:
    """Ids of existing semantic memories the new `fact` makes obsolete. Recalls
    similar facts (needs embeddings — degraded search returns no scores, so we
    supersede nothing) and asks the extractor which to replace. Best-effort."""
    if not getattr(config, "extract_supersede", True):
        return []
    res = tools.search(query=fact.text, kind="semantic",
                       top_k=getattr(config, "extract_supersede_top_k", 5))
    if not res.get("ok"):
        return []
    floor = getattr(config, "extract_supersede_min_score", 0.6)
    mems = [m for m in res.get("memories", [])
            if m.get("score") is not None and m["score"] >= floor and m.get("id")]
    if not mems:
        return []
    try:
        idxs = extractor.judge_supersedes(fact.text, [m["text"] for m in mems])
    except Exception:
        return []
    return [mems[i]["id"] for i in idxs if 0 <= i < len(mems)]


def run_capture(event: dict, config, provider: EmbeddingProvider,
                client: AegisClient | None = None) -> int:
    """Capture salient memories for an ended session. Returns the number stored.
    Never raises; backend failures result in 0 stored (FR-009).

    When LLM extraction is enabled (`extract_mode` != none) and available, the
    transcript is distilled into durable facts stored as SEMANTIC memories (so
    they participate in dedup/supersession and are protected from decay). The
    heuristic marker path is the fallback when extraction is off or fails."""
    if not config.capture_enabled:
        return 0
    texts = load_transcript(event.get("transcript_path"))

    extractor = make_extraction_provider(config)
    facts = extract_facts(texts, config, extractor)
    if facts is not None:  # extractor ran (even if it found nothing)
        if not facts:
            return 0
        if client is None:
            client = AegisClient.from_config(config)
        tools = MemoryTools(config, client, provider)
        stored = 0
        for f in facts:
            # Contradiction -> supersession: find existing memories this fact
            # updates/replaces BEFORE inserting it (so candidates are prior facts),
            # then tombstone them with a `supersedes` provenance link.
            superseded = _find_superseded_ids(f, tools, config, extractor)
            res = tools.save(f.text, tags=f.tags, importance=f.importance,
                             semantic=True, confidence=f.confidence)
            if not res.get("ok"):
                continue
            stored += 1
            new_id = res.get("id")
            for old_id in superseded:
                if old_id == new_id:
                    continue
                tools.relate(new_id, old_id, "supersedes")
                tools.delete(old_id)
        return stored

    # heuristic fallback (extraction disabled/unavailable)
    survivors = filter_candidates(derive_candidates(texts, config), config)
    if not survivors:
        return 0
    if client is None:
        client = AegisClient.from_config(config)
    tools = MemoryTools(config, client, provider)
    stored = 0
    for cand in survivors:
        res = tools.save(cand.text, tags=cand.tags, importance=cand.salience)
        if res.get("ok"):
            stored += 1
    return stored