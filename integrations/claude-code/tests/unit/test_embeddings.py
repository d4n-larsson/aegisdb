"""Unit tests for embedding providers and similarity (T013)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from aegis_mcp.embeddings import (FakeProvider, NoneProvider, VoyageProvider,
                                  make_provider, cosine)
from aegis_mcp.config import Config


class TestEmbeddings(unittest.TestCase):
    def test_none_provider(self):
        p = NoneProvider()
        self.assertFalse(p.available())
        self.assertEqual(p.dimension(), 0)

    def test_fake_provider_dimension_and_determinism(self):
        p = FakeProvider(16)
        self.assertTrue(p.available())
        self.assertEqual(p.dimension(), 16)
        v1 = p.embed_document("hello world")
        v2 = p.embed_document("hello world")
        self.assertEqual(v1, v2)             # deterministic
        self.assertEqual(len(v1), 16)

    def test_fake_provider_similarity_orders_by_overlap(self):
        p = FakeProvider(64)
        q = p.embed_query("deploy the project with make ship")
        related = p.embed_document("to deploy the project run make ship")
        unrelated = p.embed_document("the cat sat quietly on a warm mat")
        self.assertGreater(cosine(q, related), cosine(q, unrelated))

    def test_cosine_edge_cases(self):
        self.assertEqual(cosine([], []), 0.0)
        self.assertEqual(cosine([1, 0], [0, 1]), 0.0)
        self.assertAlmostEqual(cosine([1, 0], [1, 0]), 1.0)

    def test_voyage_availability_rejects_placeholder_keys(self):
        # voyageai SDK may be absent; if so, available() is False regardless and
        # there is nothing to assert about key parsing.
        try:
            import voyageai  # noqa: F401
        except ImportError:
            self.skipTest("voyageai SDK not installed")
        p = VoyageProvider()
        saved = os.environ.get("VOYAGE_API_KEY")
        try:
            for placeholder in ("", "   ", "..."):
                os.environ["VOYAGE_API_KEY"] = placeholder
                self.assertFalse(p.available(), f"{placeholder!r} should be rejected")
            os.environ["VOYAGE_API_KEY"] = "pa-real-looking-key-123"
            self.assertTrue(p.available())
        finally:
            if saved is None:
                os.environ.pop("VOYAGE_API_KEY", None)
            else:
                os.environ["VOYAGE_API_KEY"] = saved

    def test_make_provider_falls_back_to_none(self):
        # voyage selected but no SDK/key in this env -> NoneProvider
        cfg = Config(embedding_mode="voyage", embedding_dimensions=1024)
        self.assertFalse(make_provider(cfg).available())
        # none mode
        cfg2 = Config(embedding_mode="none")
        self.assertFalse(make_provider(cfg2).available())


if __name__ == "__main__":
    unittest.main()