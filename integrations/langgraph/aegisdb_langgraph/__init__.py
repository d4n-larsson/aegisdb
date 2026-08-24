"""AegisDB as a LangGraph long-term-memory store.

    from aegisdb_langgraph import AegisStore

    store = AegisStore(host="127.0.0.1", port=9470, namespace="my-agent")
    store.put(("users", "42"), "prefs", {"theme": "dark"})

See `store.py` for the mapping onto AegisDB and the three things it
deliberately does not do (TTL, vector indexing, server-side filtering).
"""
from .store import MAX_NAMESPACE_DEPTH, AegisStore

__all__ = ["AegisStore", "MAX_NAMESPACE_DEPTH"]
