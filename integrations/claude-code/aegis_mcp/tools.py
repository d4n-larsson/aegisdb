"""Core memory operations (US1, US4, US5 logic).

These pure methods implement the behaviour behind the MCP tools; ``server.py``
binds them to FastMCP. Keeping the logic here (not in the MCP binding) means it
is fully testable without the ``mcp`` SDK installed. Every method maps to one
AegisDB operation (contracts/aegisdb-mapping.md), always scopes to the
configured namespace (FR-008), and never raises — backend failures become
``{"ok": false, "error": "unavailable"}`` (FR-009).
"""
from __future__ import annotations

import sys

from .client import AegisClient, AegisUnavailable
from .embeddings import EmbeddingProvider, cosine
from . import results


def record_to_memory(rec: dict, score: float | None = None) -> dict:
    """Project an AegisDB record onto the integration's Memory shape."""
    mem = {
        "id": rec.get("id"),
        "text": rec.get("data"),
        "kind": rec.get("type"),
        "tags": rec.get("tags", []),
        "importance": rec.get("importance"),
        "confidence": rec.get("confidence"),
        "created": rec.get("created"),
        "updated": rec.get("updated"),
    }
    if score is not None:
        mem["score"] = round(score, 4)
    return mem


def score_record(rec: dict, query_embedding, *, semantic: bool) -> float | None:
    """Relevance score for a record (US5 / T041).

    Semantic: importance * confidence * cosine(similarity). Non-semantic: None
    (results keep AegisDB's native ordering).
    """
    if not semantic or query_embedding is None:
        return None
    sim = cosine(query_embedding, rec.get("embedding") or [])
    importance = rec.get("importance")
    importance = 0.5 if importance is None else float(importance)
    confidence = rec.get("confidence")
    confidence = 1.0 if confidence is None else float(confidence)
    # Blend: similarity dominates, importance/confidence modulate.
    return sim * (0.5 + 0.5 * importance) * confidence


def _suppress_near_duplicates(scored, threshold):
    """Drop a memory whose embedding is >= `threshold` cosine to an already-kept,
    higher-ranked one, so recall doesn't spend tokens re-injecting the same fact
    phrased several ways. `scored` is a list of (score, record) sorted best-first;
    the highest-scored member of each near-duplicate cluster is the one kept.
    A threshold outside (0, 1) disables the filter (returns the input unchanged).
    Records without an embedding are always kept (nothing to compare)."""
    if not 0.0 < threshold < 1.0:
        return scored
    kept, kept_vecs = [], []
    for s, r in scored:
        vec = r.get("embedding") or []
        if vec and any(cosine(vec, kv) >= threshold for kv in kept_vecs):
            continue
        kept.append((s, r))
        if vec:
            kept_vecs.append(vec)
    return kept


class MemoryTools:
    def __init__(self, config, client: AegisClient, provider: EmbeddingProvider):
        self.config = config
        self.client = client
        self.provider = provider

    # ---- helpers ---------------------------------------------------------

    def _request(self, payload: dict, read_timeout_ms: int | None = None) -> dict:
        payload.setdefault("agent_id", self.config.namespace)
        return self.client.request(payload, read_timeout_ms=read_timeout_ms)

    def _embeddings_usable(self) -> bool:
        """Whether the provider can supply correctly-sized embeddings.

        Validated once, lazily. The first call loads the model (for a local
        provider) and checks its output dimension against the configured size.
        This is done here rather than at server startup on purpose: loading a
        local model to read its dimension can stall (e.g. a Hugging Face Hub
        check), and blocking the MCP ``initialize`` handshake on it trips the
        client's startup timeout. On a mismatch, embeddings are disabled (so
        semantic search degrades) and a warning is logged, instead of silently
        sending wrong-sized vectors the backend would reject.
        """
        cached = getattr(self, "_emb_usable", None)
        if cached is not None:
            return cached
        usable = self.provider.available()
        if usable and self.provider.dimension() != self.config.embedding_dimensions:
            print(f"[aegis-mcp] embedding dimension mismatch: "
                  f"provider={self.provider.dimension()} "
                  f"config={self.config.embedding_dimensions}; disabling embeddings",
                  file=sys.stderr)
            usable = False
        self._emb_usable = usable
        return usable

    def _send(self, payload: dict, read_timeout_ms=None):
        """Send a request and translate transport/backend failures.

        Returns ``(resp, None)`` on success, or ``(None, error_result)`` if the
        backend was unreachable or returned ``ok=false`` — so callers do
        ``resp, err = self._send(...); if err: return err``.
        """
        try:
            resp = self._request(payload, read_timeout_ms=read_timeout_ms)
        except AegisUnavailable as exc:
            return None, results.unavailable(str(exc))
        if not resp.get("ok"):
            return None, results.from_aegis_error(resp)
        return resp, None

    # ---- operations ------------------------------------------------------

    def save(self, text: str, tags=None, importance: float = 0.5,
             semantic: bool = False, confidence: float = 1.0) -> dict:
        if not text or not text.strip():
            return results.err("invalid", "text must be non-empty")
        payload = {
            "operation": "insert",
            "type": "semantic" if semantic else "episodic",
            "data": text,
            "tags": list(tags or []),
            "importance": importance,
        }
        if semantic:
            payload["confidence"] = confidence
        if self._embeddings_usable():
            payload["embedding"] = self.provider.embed_document(text)
        resp, err = self._send(payload)
        if err:
            return err
        rec = resp.get("record", {})
        return results.ok(id=rec.get("id"), kind=rec.get("type"))

    def get(self, id: int) -> dict:
        resp, err = self._send({"operation": "get", "id": id})
        if err:
            return err
        return results.ok(memory=record_to_memory(resp.get("record", {})))

    def search(self, query: str | None = None, tags=None, match: str = "any",
               start_time: int | None = None, end_time: int | None = None,
               top_k: int | None = None, kind: str | None = None,
               max_importance: float | None = None,
               order: str | None = None) -> dict:
        top_k = top_k or self.config.recall_top_k
        tags = list(tags or [])
        if not query and not tags and start_time is None and end_time is None:
            return results.err("invalid", "search requires query, tags, or a time range")

        payload = {"operation": "search", "top_k": top_k}
        if tags:
            payload["tags"] = tags
            payload["match"] = match
        if start_time is not None:
            payload["start_time"] = start_time
        if end_time is not None:
            payload["end_time"] = end_time
        # Server-side candidate-selection filters (ignored by older servers, which
        # is safe: callers that rely on them also filter client-side).
        if kind is not None:
            payload["type"] = kind
        if max_importance is not None:
            payload["max_importance"] = max_importance
        if order is not None:
            payload["order"] = order

        query_embedding = None
        usable = self._embeddings_usable()
        semantic = bool(query) and usable
        degraded = bool(query) and not usable
        if semantic:
            query_embedding = self.provider.embed_query(query)
            payload["embedding"] = query_embedding

        try:
            resp = self._request(payload)
        except AegisUnavailable as exc:
            return {**results.unavailable(str(exc)), "memories": [], "total": 0,
                    "degraded": True}
        if not resp.get("ok"):
            return {**results.from_aegis_error(resp), "memories": [], "total": 0,
                    "degraded": degraded}

        records = resp.get("records", [])
        scored = [(score_record(r, query_embedding, semantic=semantic), r)
                  for r in records]
        if semantic:
            scored.sort(key=lambda s: (s[0] if s[0] is not None else 0.0),
                        reverse=True)
            scored = [(s, r) for (s, r) in scored
                      if (s or 0.0) >= self.config.recall_min_score]
            scored = _suppress_near_duplicates(
                scored, self.config.recall_dedup_threshold)
        scored = scored[:top_k]
        memories = [record_to_memory(r, score=s) for (s, r) in scored]
        return results.ok(total=len(memories), memories=memories, degraded=degraded)

    def update(self, id: int, text: str | None = None,
               confidence: float | None = None, tags=None) -> dict:
        payload = {"operation": "update", "id": id}
        if text is not None:
            payload["data"] = text
        if confidence is not None:
            payload["confidence"] = confidence
        if tags is not None:
            payload["tags"] = list(tags)
        if text is not None and self._embeddings_usable():
            payload["embedding"] = self.provider.embed_document(text)
        resp, err = self._send(payload)
        if err:
            return err
        return results.ok(memory=record_to_memory(resp.get("record", {})))

    def relate(self, from_id: int, to_id: int, kind: str | None = None) -> dict:
        payload = {"operation": "relate", "from_id": from_id, "to_id": to_id}
        if kind:
            payload["kind"] = kind
        resp, err = self._send(payload)
        if err:
            return err
        return results.ok(relationship=resp.get("relationship"))

    def delete(self, id: int) -> dict:
        """Tombstone a record: dropped from recall, reclaimed by compaction, but
        recoverable from the log until then. Used to archive summarized sources."""
        resp, err = self._send({"operation": "delete", "id": id})
        if err:
            return err
        return results.ok(id=id, deleted=resp.get("deleted", True))