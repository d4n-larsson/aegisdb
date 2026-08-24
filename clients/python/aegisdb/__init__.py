"""AegisDB — a client for the newline-delimited JSON protocol.

    from aegisdb import AegisClient

    with AegisClient(host="127.0.0.1", port=9470) as db:
        rec = db.insert("prefers dark mode", type="semantic", tags=["user"])
        hits = db.search(query="dark mode", top_k=5)

Standard library only. See https://github.com/d4n-larsson/aegisdb for the
server and the full wire-protocol reference.
"""
from .client import DEFAULT_HOST, DEFAULT_PORT, AegisClient
from .errors import (AegisError, AegisRequestError, AegisUnavailable,
                     Forbidden, Immutable, InternalError, InvalidRequest,
                     MemoryLimit, NotFound, NotReady, PayloadTooLarge,
                     QuotaExceeded, RateLimited, ReadOnly, Unauthorized)

__all__ = [
    "AegisClient", "DEFAULT_HOST", "DEFAULT_PORT",
    "AegisError", "AegisUnavailable", "AegisRequestError",
    "InvalidRequest", "NotFound", "PayloadTooLarge", "Immutable", "NotReady",
    "Unauthorized", "Forbidden", "QuotaExceeded", "RateLimited", "ReadOnly",
    "MemoryLimit", "InternalError",
]
