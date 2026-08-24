"""The adapter against a real aegisdb, driven through BaseStore's own surface.

Deliberately *not* against `batch` directly: the concrete `get`/`put`/`search`/
`delete`/`list_namespaces` on the base class are what a graph actually calls,
and exercising them is what proves the op types are dispatched the way
LangGraph builds them.

Where a behaviour is meant to match the reference implementation, the same
case is run against `InMemoryStore` and the two are compared — an assertion
written from memory of the contract would just encode my reading of it.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

REPO = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))
BINARY = os.path.join(REPO, "build", "aegisdb")

try:
    from langgraph.store.memory import InMemoryStore
    from aegisdb_langgraph import MAX_NAMESPACE_DEPTH, AegisStore
    from aegisdb_langgraph.store import _h, _supports_native_filter
    DEPS = True
except ImportError:
    DEPS = False

# The graph runtime is a *dev* dependency: the package needs only
# langgraph-checkpoint, which is where BaseStore lives. So the end-to-end test
# below skips rather than failing when only the runtime half is missing.
try:
    from langgraph.graph import END, START, StateGraph
    HAS_GRAPH = True
except ImportError:
    HAS_GRAPH = False


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, extra=None):
        self.port = _free_port()
        self.datadir = tempfile.mkdtemp(prefix="aegis_lg_")
        self.extra = list(extra or [])

    def __enter__(self):
        self.proc = subprocess.Popen(
            [BINARY, "--data-dir", self.datadir, "--port", str(self.port),
             "--embedding-dim", "8"] + self.extra,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(60):
            try:
                with socket.create_connection(("127.0.0.1", self.port),
                                              timeout=0.2):
                    return self
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("aegisdb did not start")

    def __exit__(self, *exc):
        self.proc.kill()
        self.proc.wait()
        return False


@unittest.skipUnless(DEPS, "langgraph-checkpoint / aegisdb not installed")
@unittest.skipUnless(os.path.exists(BINARY), "aegisdb binary not built")
class TestAegisStore(unittest.TestCase):
    def store(self, srv, **kw):
        """A store closed when the test ends.

        The client reuses one connection, so a test that forgets leaks a
        socket — which is how these first ran, with ResourceWarnings to show
        for it.
        """
        s = AegisStore(port=srv.port, namespace="lg-test", **kw)
        self.addCleanup(s.close)
        return s

    # ---- the basics, compared against the reference store ----------------

    def test_put_get_roundtrip_matches_the_reference(self):
        with Server() as srv:
            for store in (self.store(srv), InMemoryStore()):
                store.put(("users", "42"), "prefs", {"theme": "dark", "n": 3})
                item = store.get(("users", "42"), "prefs")
                self.assertEqual(item.value, {"theme": "dark", "n": 3},
                                 type(store).__name__)
                self.assertEqual(item.key, "prefs")
                self.assertEqual(item.namespace, ("users", "42"))

    def test_a_missing_item_is_none_not_an_error(self):
        with Server() as srv:
            for store in (self.store(srv), InMemoryStore()):
                self.assertIsNone(store.get(("nope",), "nothing"),
                                  type(store).__name__)

    def test_put_overwrites_in_place(self):
        with Server() as srv:
            store = self.store(srv)
            store.put(("a",), "k", {"v": 1})
            first = store.get(("a",), "k")
            store.put(("a",), "k", {"v": 2})
            self.assertEqual(store.get(("a",), "k").value, {"v": 2})
            # One item, not two: an overwrite that inserted would leave the old
            # value discoverable by search.
            self.assertEqual(len(store.search(("a",))), 1)
            self.assertEqual(store.get(("a",), "k").created_at,
                             first.created_at, "the record survived, not a copy")

    def test_delete(self):
        with Server() as srv:
            for store in (self.store(srv), InMemoryStore()):
                store.put(("a",), "k", {"v": 1})
                store.delete(("a",), "k")
                self.assertIsNone(store.get(("a",), "k"),
                                  type(store).__name__)

    def test_delete_of_a_missing_key_is_a_no_op(self):
        with Server() as srv:
            for store in (self.store(srv), InMemoryStore()):
                store.delete(("a",), "never-existed")  # must not raise

    # ---- namespaces ------------------------------------------------------

    def test_search_matches_by_prefix_not_by_exact_namespace(self):
        with Server() as srv:
            for store in (self.store(srv), InMemoryStore()):
                store.put(("users", "1", "prefs"), "a", {"v": 1})
                store.put(("users", "2", "prefs"), "b", {"v": 2})
                store.put(("orgs", "1"), "c", {"v": 3})
                label = type(store).__name__
                self.assertEqual(len(store.search(("users",))), 2, label)
                self.assertEqual(len(store.search(("users", "1"))), 1, label)
                self.assertEqual(len(store.search(())), 3, label)
                self.assertEqual(len(store.search(("nobody",))), 0, label)

    def test_namespaces_are_paths_not_joined_strings(self):
        """("a", "bc") and ("ab", "c") are different namespaces. A separator-
        joined encoding makes them the same string, and then one namespace's
        search returns the other's items."""
        with Server() as srv:
            store = self.store(srv)
            store.put(("a", "bc"), "k", {"which": "first"})
            store.put(("ab", "c"), "k", {"which": "second"})
            self.assertEqual(store.get(("a", "bc"), "k").value,
                             {"which": "first"})
            self.assertEqual(store.get(("ab", "c"), "k").value,
                             {"which": "second"})
            self.assertNotEqual(_h(("a", "bc")), _h(("ab", "c")))

    def test_list_namespaces_matches_the_reference(self):
        with Server() as srv:
            results = {}
            for store in (self.store(srv), InMemoryStore()):
                for ns in (("users", "1"), ("users", "2"), ("orgs", "1"),
                           ("users", "1", "deep")):
                    store.put(ns, "k", {"v": 1})
                results[type(store).__name__] = {
                    "all": store.list_namespaces(),
                    "prefix": store.list_namespaces(prefix=("users",)),
                    "suffix": store.list_namespaces(suffix=("1",)),
                    "depth1": store.list_namespaces(max_depth=1),
                }
            a, b = results.values()
            for key in ("all", "prefix", "suffix", "depth1"):
                self.assertEqual(sorted(a[key]), sorted(b[key]),
                                 f"{key}: {a[key]} != {b[key]}")

    def test_a_namespace_deeper_than_the_tag_budget_is_refused(self):
        """32 tags per record, and this encoding needs one per prefix. Refused
        with a message naming the ceiling, rather than an INVALID_REQUEST from
        the server that says nothing about namespaces."""
        with Server() as srv:
            store = self.store(srv)
            deep = tuple(str(i) for i in range(MAX_NAMESPACE_DEPTH + 1))
            with self.assertRaises(ValueError) as caught:
                store.put(deep, "k", {"v": 1})
            self.assertIn("namespace", str(caught.exception))
            ok = tuple(str(i) for i in range(MAX_NAMESPACE_DEPTH))
            store.put(ok, "k", {"v": 1})  # the ceiling itself must work
            self.assertEqual(store.get(ok, "k").value, {"v": 1})

    # ---- search ----------------------------------------------------------

    def test_query_ranks_by_the_servers_keyword_index(self):
        with Server() as srv:
            store = self.store(srv)
            store.put(("n",), "a", {"note": "the deploy runbook lives in ops"})
            store.put(("n",), "b", {"note": "unrelated thoughts about coffee"})
            hits = store.search(("n",), query="runbook")
            self.assertEqual([h.key for h in hits], ["a"], hits)

    def test_query_without_the_lexical_index_says_which_flag(self):
        with Server(extra=["--no-lexical-index"]) as srv:
            store = self.store(srv)
            store.put(("n",), "a", {"note": "x"})
            with self.assertRaises(RuntimeError) as caught:
                store.search(("n",), query="x")
            self.assertIn("--no-lexical-index", str(caught.exception))

    def test_filter_matches_the_reference_including_operators(self):
        self.assertTrue(_supports_native_filter,
                        "langgraph moved _compare_values; the adapter would "
                        "fall back to exact-match-only filtering")
        # The operator set langgraph actually implements ($in and friends are
        # not among them), plus a nested-dict compare and a miss.
        cases = [{"lang": "py"}, {"n": {"$gt": 1}}, {"n": {"$ne": 2}},
                 {"n": {"$gte": 2, "$lte": 3}}, {"lang": {"$eq": "rs"}},
                 {"meta": {"tier": "a"}}, {"missing": "x"}]
        with Server() as srv:
            got = {}
            for store in (self.store(srv), InMemoryStore()):
                for i, lang in enumerate(("py", "py", "rs"), start=1):
                    store.put(("f",), f"k{i}",
                              {"lang": lang, "n": i,
                               "meta": {"tier": "a" if i < 3 else "b"}})
                got[type(store).__name__] = [
                    sorted(h.key for h in store.search(("f",), filter=f))
                    for f in cases]
            a, b = got.values()
            self.assertEqual(a, b, f"{a} != {b}")

    def test_an_unsupported_operator_fails_the_same_way(self):
        """Reusing langgraph's comparator means its limits are inherited too —
        `$in` is not implemented there, and this store must refuse it exactly
        as the reference does rather than quietly accepting a filter the graph
        would get different answers from elsewhere."""
        with Server() as srv:
            for store in (self.store(srv), InMemoryStore()):
                store.put(("f",), "k", {"n": 1})
                with self.assertRaises(ValueError, msg=type(store).__name__):
                    store.search(("f",), filter={"n": {"$in": [1]}})

    def test_limit_and_offset_page_matches_the_reference(self):
        with Server() as srv:
            pages = {}
            for store in (self.store(srv), InMemoryStore()):
                for i in range(5):
                    store.put(("p",), f"k{i}", {"i": i})
                pages[type(store).__name__] = [
                    len(store.search(("p",), limit=2)),
                    len(store.search(("p",), limit=2, offset=2)),
                    len(store.search(("p",), limit=2, offset=4)),
                    len(store.search(("p",), limit=10, offset=10)),
                ]
            a, b = pages.values()
            self.assertEqual(a, b, f"{a} != {b}")

    def test_filtered_paging_slices_after_filtering(self):
        """Paging a filtered search on the server would skip rows before they
        were filtered, so page 2 would silently omit matches."""
        with Server() as srv:
            store = self.store(srv)
            for i in range(6):
                store.put(("q",), f"k{i}", {"keep": i % 2 == 0, "i": i})
            keep = {"keep": True}
            first = store.search(("q",), filter=keep, limit=2)
            second = store.search(("q",), filter=keep, limit=2, offset=2)
            self.assertEqual(len(first), 2)
            self.assertEqual(len(second), 1, "three match in total")
            self.assertEqual(
                len({h.key for h in first} | {h.key for h in second}), 3,
                "the pages are disjoint and cover every match")

    # ---- the refusals ----------------------------------------------------

    def test_ttl_is_refused_not_ignored(self):
        """Two layers, both of which must refuse.

        `supports_ttl = False` makes LangGraph's own `put` raise before the op
        is ever built, which is the path a graph takes. A direct `batch` call
        skips that check, so the adapter guards it too — and says *why*, since
        the framework's message cannot know that `forget` is the mechanism
        that applies here.
        """
        from langgraph.store.base import PutOp

        with Server() as srv:
            store = self.store(srv)
            with self.assertRaises(NotImplementedError):
                store.put(("a",), "k", {"v": 1}, ttl=5.0)

            with self.assertRaises(NotImplementedError) as caught:
                store.batch([PutOp(namespace=("a",), key="k",
                                   value={"v": 1}, ttl=5.0)])
            self.assertIn("forget", str(caught.exception))
            self.assertIsNone(store.get(("a",), "k"),
                              "a refused put must not have written")

    def test_index_is_accepted_and_ignored(self):
        """`index=` selects fields to embed. Honouring it would mean owning an
        embeddings function; accepting it keeps graphs that set it working."""
        with Server() as srv:
            store = self.store(srv)
            store.put(("a",), "k", {"text": "hello"}, index=["text"])
            self.assertEqual(store.get(("a",), "k").value, {"text": "hello"})
            store.put(("a",), "j", {"text": "hi"}, index=False)
            self.assertEqual(store.get(("a",), "j").value, {"text": "hi"})

    # ---- isolation and async --------------------------------------------

    def test_two_stores_in_different_aegisdb_namespaces_are_isolated(self):
        with Server() as srv:
            a = AegisStore(port=srv.port, namespace="tenant-a")
            b = AegisStore(port=srv.port, namespace="tenant-b")
            self.addCleanup(a.close)
            self.addCleanup(b.close)
            a.put(("shared",), "k", {"whose": "a"})
            self.assertEqual(a.get(("shared",), "k").value, {"whose": "a"})
            self.assertIsNone(b.get(("shared",), "k"),
                              "the server's namespace isolation holds")
            self.assertEqual(b.list_namespaces(), [])

    def test_the_store_is_a_context_manager(self):
        with Server() as srv:
            with AegisStore(port=srv.port, namespace="ctx") as store:
                store.put(("a",), "k", {"v": 1})
                self.assertEqual(store.get(("a",), "k").value, {"v": 1})
            # Closed, but still usable: the client reconnects on demand, so
            # close() releases a socket rather than poisoning the object.
            self.assertEqual(store.get(("a",), "k").value, {"v": 1})
            store.close()

    def test_the_async_surface_works(self):
        import asyncio

        with Server() as srv:
            store = self.store(srv)

            async def go():
                await store.aput(("a",), "k", {"v": 1})
                item = await store.aget(("a",), "k")
                found = await store.asearch(("a",))
                return item, found

            item, found = asyncio.run(go())
            self.assertEqual(item.value, {"v": 1})
            self.assertEqual(len(found), 1)

    def test_a_foreign_record_in_the_namespace_is_ignored(self):
        """The store shares its AegisDB namespace with whatever else writes
        there. A record that is not ours must not surface as an item with a
        garbled value."""
        from aegisdb import AegisClient

        with Server() as srv:
            store = self.store(srv)
            store.put(("a",), "k", {"v": 1})
            with AegisClient(port=srv.port, agent_id="lg-test") as other:
                other.insert("just some prose nobody encoded", type="semantic",
                             tags=["something-else"])
            self.assertEqual(len(store.search(())), 1)
            self.assertEqual(store.list_namespaces(), [("a",)])


@unittest.skipUnless(DEPS, "langgraph-checkpoint / aegisdb not installed")
@unittest.skipUnless(os.path.exists(BINARY), "aegisdb binary not built")
class TestReviewFindings(unittest.TestCase):
    """Cases the first round of tests passed straight through.

    Each of these is a real defect the suite above could not see, kept so the
    same blind spot cannot reopen.
    """

    def store(self, srv, **kw):
        s = AegisStore(port=srv.port, namespace="lg-fix", **kw)
        self.addCleanup(s.close)
        return s

    def test_search_with_a_query_returns_a_score(self):
        """The server emits a score only when asked to explain, so this was
        always None — every ranking or thresholding caller got nothing."""
        with Server() as srv:
            store = self.store(srv)
            store.put(("n",), "a", {"note": "the deploy runbook lives in ops"})
            hits = store.search(("n",), query="runbook")
            self.assertEqual(len(hits), 1)
            self.assertIsNotNone(hits[0].score, "a ranked hit carries a score")
            self.assertGreater(hits[0].score, 0)

    def test_a_queryless_search_is_not_blamed_on_the_lexical_index(self):
        """NOT_READY has causes besides a missing BM25 index. Rewriting every
        one into "drop the query" points a reader at a query they never
        passed."""
        with Server(extra=["--no-lexical-index"]) as srv:
            store = self.store(srv)
            store.put(("n",), "a", {"v": 1})
            # No query: this must work, not raise about an index it never used.
            self.assertEqual(len(store.search(("n",))), 1)

    def test_deep_pages_are_not_silently_empty(self):
        """The server clamps top_k to 1000 without saying so. Fetching
        limit+offset and slicing meant any page past 1000 came back empty,
        which a caller reads as "no more results" while items remain."""
        with Server() as srv:
            store = self.store(srv)
            for i in range(12):
                store.put(("p",), f"k{i:02d}", {"i": i})
            deep = store.search(("p",), limit=2, offset=10)
            self.assertEqual(len(deep), 2, "the last page is served, not empty")
            self.assertEqual(len(store.search(("p",), limit=20)), 12,
                             "one page over the whole set is exact")
            # NOT asserted: that offset paging covers every item exactly once.
            # An unranked search is ordered by `created` alone with a
            # non-stable sort, and these twelve land in ~3 distinct
            # milliseconds, so a page boundary inside a tie group can skip one
            # and repeat another. That is the server's ordering, not this
            # adapter's paging — asserting it here would be asserting a
            # guarantee nothing makes, and the test would flake (it did, about
            # one run in three).

    def test_scan_limit_is_clamped_to_what_the_server_will_serve(self):
        """Above 1000 the server returns fewer and says nothing, so a larger
        value would read as "scans more" while changing nothing."""
        with Server() as srv:
            store = self.store(srv, search_scan_limit=50_000)
            self.assertEqual(store.search_scan_limit, AegisStore.MAX_PAGE)

    def test_an_empty_suffix_matches_everything(self):
        """`ns[-0:]` is the whole tuple, not an empty slice — so an empty
        suffix matched nothing where InMemoryStore matches all."""
        with Server() as srv:
            for store in (self.store(srv), InMemoryStore()):
                store.put(("users", "1"), "k", {"v": 1})
                store.put(("orgs",), "k", {"v": 2})
                self.assertEqual(sorted(store.list_namespaces(suffix=())),
                                 sorted(store.list_namespaces()),
                                 type(store).__name__)

    def test_a_client_and_a_conflicting_namespace_is_refused(self):
        """Isolation comes from the client. Reporting a different namespace
        than the one data lands in is a lie about where it went."""
        from aegisdb import AegisClient

        with Server() as srv:
            c = AegisClient(port=srv.port, agent_id="from-client")
            self.addCleanup(c.close)
            with self.assertRaises(ValueError):
                AegisStore(client=c, namespace="different")
            store = AegisStore(client=c)
            self.assertEqual(store.namespace, "from-client")

    def test_concurrent_use_does_not_interleave_on_one_socket(self):
        """The client owns a socket and is not thread-safe; `abatch` runs on a
        worker thread and LangGraph's sync runner uses a pool. Unserialised,
        two threads interleave sendall/recv and one reads the other's
        response — which surfaces as a get returning another key's record."""
        import concurrent.futures as cf

        with Server() as srv:
            store = self.store(srv)
            for i in range(20):
                store.put(("c",), f"k{i}", {"i": i})

            def read(i):
                item = store.get(("c",), f"k{i}")
                return item.value["i"] if item else None

            with cf.ThreadPoolExecutor(max_workers=8) as pool:
                got = list(pool.map(read, list(range(20)) * 5))
            self.assertEqual(got, list(range(20)) * 5,
                             "every read returned its own record")


@unittest.skipUnless(DEPS and HAS_GRAPH, "langgraph runtime not installed")
@unittest.skipUnless(os.path.exists(BINARY), "aegisdb binary not built")
class TestInsideAGraph(unittest.TestCase):
    """The integration point, not just the interface.

    Everything above drives `BaseStore`'s methods directly. This compiles a
    real graph with `store=` and lets LangGraph inject it into the nodes, which
    is how anyone actually uses this — and is the part that would break if the
    class satisfied the ABC but not the runtime's expectations of it.
    """

    def test_a_compiled_graph_writes_and_reads_through_the_store(self):
        from typing import TypedDict

        class S(TypedDict):
            user: str
            seen: str

        def remember(state, *, store):
            store.put(("users", state["user"]), "last", {"seen": "hello"})
            return state

        def recall(state, *, store):
            item = store.get(("users", state["user"]), "last")
            return {**state, "seen": item.value["seen"] if item else "nothing"}

        with Server() as srv:
            store = AegisStore(port=srv.port, namespace="graph-test")
            self.addCleanup(store.close)
            g = StateGraph(S)
            g.add_node("remember", remember)
            g.add_node("recall", recall)
            g.add_edge(START, "remember")
            g.add_edge("remember", "recall")
            g.add_edge("recall", END)
            app = g.compile(store=store)

            out = app.invoke({"user": "42", "seen": ""})
            self.assertEqual(out["seen"], "hello")
            # And it is really in AegisDB, not in some runtime cache.
            self.assertEqual(
                [(i.namespace, i.key, i.value) for i in store.search(())],
                [(("users", "42"), "last", {"seen": "hello"})])


if __name__ == "__main__":
    unittest.main()
