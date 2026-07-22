"""Embedders for the recall-quality eval harness.

The AegisDB server does not compute embeddings — clients supply them — so the
eval must produce vectors for both stored memories and queries. The default is a
dependency-free deterministic *hashing* embedder so the harness runs anywhere
(CI, offline) and is fully reproducible. A real embedding model can be swapped in
later behind the same `embed(text) -> list[float]` seam without touching the
runner; see `resolve_embedder`.
"""
from __future__ import annotations

import json
import math
import re
import subprocess
from typing import Callable, List

_TOKEN = re.compile(r"[a-z0-9]+")


def _tokens(text: str) -> List[str]:
    return [t for t in _TOKEN.findall(text.lower()) if len(t) > 1]


def _h(token: str, salt: str) -> int:
    # FNV-1a over (salt|token); deterministic across runs and platforms.
    h = 1469598103934665603
    for ch in f"{salt}\x00{token}".encode():
        h ^= ch
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def hashing_embedder(dim: int) -> Callable[[str], List[float]]:
    """A hashing / random-indexing embedder: each token is hashed to a bucket
    with a sign, accumulated, then L2-normalized. Documents that share vocabulary
    land near each other in cosine space — enough signal to make recall
    measurable and to catch scoring regressions, while staying deterministic."""

    def embed(text: str) -> List[float]:
        vec = [0.0] * dim
        for tok in _tokens(text):
            bucket = _h(tok, "b") % dim
            sign = 1.0 if (_h(tok, "s") & 1) else -1.0
            vec[bucket] += sign
        norm = math.sqrt(sum(x * x for x in vec))
        if norm > 0:
            vec = [x / norm for x in vec]
        else:
            # Empty/degenerate text: a fixed unit vector avoids a zero query that
            # every record ties on.
            vec[0] = 1.0
        return vec

    return embed


def command_embedder(dim: int, cmd: str) -> Callable[[str], List[float]]:
    """Shell out to an external embedder for higher-fidelity runs. `cmd` receives
    the text on stdin and must print a JSON array of exactly `dim` floats. Kept
    intentionally simple; a real model backend can wrap this contract."""

    def embed(text: str) -> List[float]:
        out = subprocess.run(
            cmd, shell=True, input=text.encode(), capture_output=True, check=True
        )
        vec = json.loads(out.stdout.decode())
        if not isinstance(vec, list) or len(vec) != dim:
            raise ValueError(f"embedder returned {len(vec) if isinstance(vec, list) else '?'} dims, expected {dim}")
        return [float(x) for x in vec]

    return embed


def resolve_embedder(name: str, dim: int, cmd: str | None = None) -> Callable[[str], List[float]]:
    if name == "hashing":
        return hashing_embedder(dim)
    if name == "command":
        if not cmd:
            raise ValueError("--embedder command requires --embedder-cmd")
        return command_embedder(dim, cmd)
    raise ValueError(f"unknown embedder: {name}")