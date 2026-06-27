"""Core memory operations (US1, US4, US5 logic).

These pure methods implement the behaviour behind the MCP tools; ``server.py``
binds them to FastMCP. Keeping the logic here (not in the MCP binding) means it
is fully testable without the ``mcp`` SDK installed. Every method maps to one
AegisDB operation (contracts/aegisdb-mapping.md), always scopes to the
configured namespace (FR-008), and never raises — backend failures become
``{"ok": false, "error": "unavailable"}`` (FR-009).
"""
from __future__ import annotations

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


class MemoryTools:
    def __init__(self, config, client: AegisClient, provider: EmbeddingProvider):
        self.config = config
        self.client = client
        self.provider = provider

    # ---- helpers ---------------------------------------------------------

    def _request(self, payload: dict, read_timeout_ms: int | None = None) -> dict:
        payload.setdefault("agent_id", self.config.namespace)
        return self.client.request(payload, read_timeout_ms=read_timeout_ms)

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
        if self.provider.available():
            payload["embedding"] = self.provider.embed_document(text)
        try:
            resp = self._request(payload)
        except AegisUnavailable as exc:
            return results.unavailable(str(exc))
        if not resp.get("ok"):
            return results.from_aegis_error(resp)
        rec = resp.get("record", {})
        return results.ok(id=rec.get("id"), kind=rec.get("type"))

    def get(self, id: int) -> dict:
        try:
            resp = self._request({"operation": "get", "id": id})
        except AegisUnavailable as exc:
            return results.unavailable(str(exc))
        if not resp.get("ok"):
            return results.from_aegis_error(resp)
        return results.ok(memory=record_to_memory(resp.get("record", {})))

    def search(self, query: str | None = None, tags=None, match: str = "any",
               start_time: int | None = None, end_time: int | None = None,
               top_k: int | None = None) -> dict:
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

        query_embedding = None
        semantic = bool(query) and self.provider.available()
        degraded = bool(query) and not self.provider.available()
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
        if text is not None and self.provider.available():
            payload["embedding"] = self.provider.embed_document(text)
        try:
            resp = self._request(payload)
        except AegisUnavailable as exc:
            return results.unavailable(str(exc))
        if not resp.get("ok"):
            return results.from_aegis_error(resp)
        return results.ok(memory=record_to_memory(resp.get("record", {})))

    def relate(self, from_id: int, to_id: int, kind: str | None = None) -> dict:
        payload = {"operation": "relate", "from_id": from_id, "to_id": to_id}
        if kind:
            payload["kind"] = kind
        try:
            resp = self._request(payload)
        except AegisUnavailable as exc:
            return results.unavailable(str(exc))
        if not resp.get("ok"):
            return results.from_aegis_error(resp)
        return results.ok(relationship=resp.get("relationship"))