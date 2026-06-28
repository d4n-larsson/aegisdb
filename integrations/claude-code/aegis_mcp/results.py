"""Structured tool results and AegisDB error translation (T008).

Every tool returns a plain dict the model can read: ``{"ok": true, ...}`` or
``{"ok": false, "error": <code>, "message": <text>}`` (FR-015). AegisDB wire
error codes are mapped to the integration's stable error vocabulary
(contracts/aegisdb-mapping.md).
"""
from __future__ import annotations

# AegisDB wire-protocol error code -> integration error code.
AEGIS_ERROR_MAP = {
    "NOT_FOUND": "not_found",
    "INVALID_REQUEST": "invalid",
    "PAYLOAD_TOO_LARGE": "payload_too_large",
    "IMMUTABLE": "immutable",
    "NOT_READY": "unavailable",
    "UNAUTHORIZED": "unauthorized",
    "FORBIDDEN": "forbidden",
    "INTERNAL": "unavailable",
}


def ok(**fields) -> dict:
    return {"ok": True, **fields}


def err(code: str, message: str) -> dict:
    return {"ok": False, "error": code, "message": message}


def from_aegis_error(resp: dict) -> dict:
    """Translate an AegisDB ``{"ok": false, "error": {...}}`` response."""
    e = resp.get("error") or {}
    wire_code = e.get("code", "INTERNAL")
    code = AEGIS_ERROR_MAP.get(wire_code, "unavailable")
    message = e.get("message", "backend error")
    # A failed embedded insert/search most often means a dimension mismatch.
    if wire_code == "INVALID_REQUEST" and "embedding" in message.lower():
        message += " (check that AEGIS_EMBEDDING_DIMENSIONS matches the server's --embedding-dim)"
    return err(code, message)


def unavailable(detail: str = "memory backend unavailable") -> dict:
    return err("unavailable", detail)