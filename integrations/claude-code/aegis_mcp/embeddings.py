"""Embedding provider abstraction (T007) + providers (T038–T040).

One interface, selected by config:
  - ``none``   : embeddings disabled; semantic search falls back to tags/time.
  - ``voyage`` : Voyage AI (Anthropic's recommended provider).
  - ``local``  : a local sentence-transformer model (offline).
  - fake       : deterministic, dependency-free; used by tests.

Optional third-party SDKs are imported lazily inside each provider so importing
this module never requires them.
"""
from __future__ import annotations

import math
import re

_TOKEN = re.compile(r"[a-z0-9]+")


def _tokens(text: str):
    return _TOKEN.findall(text.lower())


def _looks_like_key(value) -> bool:
    """Reject values that obviously aren't API keys.

    A key cannot be validated offline, so this only filters out the cases that
    would otherwise be mistaken for a configured key: missing, whitespace-only,
    or punctuation placeholders such as ``"..."``. A real-looking but wrong key
    still passes here and surfaces as a clear error on first use (translated in
    results.py). This keeps ``available()`` honest so the provider degrades to
    NoneProvider (FR-011) instead of hard-failing on a placeholder."""
    if not value:
        return False
    return any(ch.isalnum() for ch in value.strip())


class EmbeddingProvider:
    """Base interface. Subclasses override the methods they support."""

    def available(self) -> bool:
        return False

    def dimension(self) -> int:
        raise NotImplementedError

    def embed_document(self, text: str) -> list[float]:
        raise NotImplementedError

    def embed_query(self, text: str) -> list[float]:
        # Default: queries embed like documents unless a provider distinguishes.
        return self.embed_document(text)


class NoneProvider(EmbeddingProvider):
    """No embeddings: semantic search is disabled (FR-011)."""

    def available(self) -> bool:
        return False

    def dimension(self) -> int:
        return 0


class FakeProvider(EmbeddingProvider):
    """Deterministic hashing-trick bag-of-words embedding.

    Each token is hashed into one of ``dim`` buckets and accumulated, then the
    vector is L2-normalised. Texts that share tokens get higher cosine
    similarity, so ranking behaviour is meaningful and reproducible in tests
    without any model or network.
    """

    def __init__(self, dim: int = 16):
        self._dim = dim

    def available(self) -> bool:
        return True

    def dimension(self) -> int:
        return self._dim

    def embed_document(self, text: str) -> list[float]:
        vec = [0.0] * self._dim
        for tok in _tokens(text):
            # Stable, process-independent bucket (avoid hash() randomisation).
            h = 0
            for ch in tok:
                h = (h * 131 + ord(ch)) & 0xFFFFFFFF
            vec[h % self._dim] += 1.0
        norm = math.sqrt(sum(v * v for v in vec))
        if norm > 0:
            vec = [v / norm for v in vec]
        return vec


class VoyageProvider(EmbeddingProvider):
    """Voyage AI provider (T038). Requires the optional ``voyageai`` SDK and a
    ``VOYAGE_API_KEY``. Output dimension is pinned to the configured value via
    Voyage's Matryoshka ``output_dimension`` so it always matches the server."""

    def __init__(self, model: str = "voyage-3-large", dim: int = 1024):
        self._model = model
        self._dim = dim
        self._client = None

    def _ensure(self):
        if self._client is None:
            import voyageai  # lazy: only needed when this provider is selected
            self._client = voyageai.Client()
        return self._client

    def available(self) -> bool:
        try:
            import voyageai  # noqa: F401
        except ImportError:
            return False
        import os
        return _looks_like_key(os.environ.get("VOYAGE_API_KEY"))

    def dimension(self) -> int:
        return self._dim

    def _embed(self, text: str, input_type: str) -> list[float]:
        client = self._ensure()
        res = client.embed([text], model=self._model, input_type=input_type,
                            output_dimension=self._dim)
        return list(res.embeddings[0])

    def embed_document(self, text: str) -> list[float]:
        return self._embed(text, "document")

    def embed_query(self, text: str) -> list[float]:
        return self._embed(text, "query")


class LocalProvider(EmbeddingProvider):
    """Local sentence-transformer provider (T039). Requires the optional
    ``sentence-transformers`` extra; fully offline."""

    def __init__(self, model: str = "all-MiniLM-L6-v2"):
        self._model_name = model
        self._model = None

    def _ensure(self):
        if self._model is None:
            from sentence_transformers import SentenceTransformer  # lazy
            self._model = SentenceTransformer(self._model_name)
        return self._model

    def available(self) -> bool:
        try:
            import sentence_transformers  # noqa: F401
        except ImportError:
            return False
        return True

    def dimension(self) -> int:
        # Report the model's true output size so a config/server mismatch is
        # caught at startup rather than silently producing wrong-sized vectors.
        return int(self._ensure().get_sentence_embedding_dimension())

    def embed_document(self, text: str) -> list[float]:
        model = self._ensure()
        return [float(x) for x in model.encode(text)]


def make_provider(config) -> EmbeddingProvider:
    """Provider factory (T040). Falls back to NoneProvider when the configured
    provider is unavailable (missing SDK or key), so semantic search degrades
    rather than failing (FR-011)."""
    mode = (config.embedding_mode or "none").lower()
    if mode == "fake":
        # Deterministic, dependency-free provider for local dev and tests.
        return FakeProvider(config.embedding_dimensions)
    if mode == "voyage":
        p = VoyageProvider(config.embedding_model, config.embedding_dimensions)
        return p if p.available() else NoneProvider()
    if mode == "local":
        # Uses the local default model (all-MiniLM-L6-v2, 384-dim); set
        # AEGIS_EMBEDDING_DIMENSIONS=384 and the server's --embedding-dim to match.
        p = LocalProvider()
        return p if p.available() else NoneProvider()
    return NoneProvider()


def cosine(a, b) -> float:
    if not a or not b or len(a) != len(b):
        return 0.0
    dot = sum(x * y for x, y in zip(a, b))
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(y * y for y in b))
    if na == 0 or nb == 0:
        return 0.0
    return dot / (na * nb)