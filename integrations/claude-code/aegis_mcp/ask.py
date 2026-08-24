"""The read path: a question becomes a `pattern`, a proof becomes English.

ROADMAP 5.4 §5, the mirror of `triples.py`. That module turns prose into typed
facts on the way in; this one turns a question into a lookup over them on the
way out, and a derivation into something a person can read.

Three properties, and the first is the one everything else is arranged around:

- **Strictly an addition.** Every way this can fail — the feature is off, no
  registry, the model declines, the predicate is undeclared, the subject does
  not resolve, the lookup returns nothing — falls back to the retrieval that
  runs today. No question that is answerable now becomes unanswerable, which is
  the `--no-lexical-index` discipline the ground rules ask for.
- **Grounding on read resolves; it never mints.** `triples.py` mints because a
  fact about an unknown thing needs somewhere to hang. A *question* about an
  unknown thing has no answer, and minting one would let reading the store
  write to it — an entity record per unrecognised noun phrase, created by
  people asking questions, indistinguishable afterwards from one somebody
  asserted.
- **The model reads the proof; it never produces it.** A verbalization is
  rendered from a derivation the server computed and that can be checked
  against the record. It is attached *beside* the payload, never in place of
  it, and the payload handed to the provider is a copy — so a backend that
  edits what it is given changes nothing that anyone reads. If the prose and
  the derivation disagree, the derivation is right.
"""
from __future__ import annotations

from .grounding import resolve

# A derivation names at most DERIV_MAX_PREMISES ids per route, and a
# verbalization reads one route. Fetching premise prose is one `get` each, on a
# path a person is waiting on, so it is bounded rather than trusted to be small.
_MAX_PREMISES_READ = 8


def _retrieval(tools, question: str, top_k: int | None, config=None) -> dict:
    """The path that runs today. Every fallback lands here.

    Derivations are asked for when verbalization is on, because the two
    features are independent: a derived record can surface through ordinary
    retrieval, and it deserves its "because" whether or not the question
    happened to be expressible as a pattern.
    """
    res = tools.search(query=question, top_k=top_k, lexical=True,
                       derivations=bool(getattr(config, "ask_verbalize", False)))
    res["symbolic"] = False
    return res


def formulate(question: str, vocab, config, extractor) -> dict | None:
    """A question as a pattern of *mentions*, or None to fall back.

    Never raises: an unavailable backend is a fallback, not an error.
    """
    if not getattr(config, "ask_pattern", False) or not vocab:
        return None
    try:
        pat = extractor.formulate_pattern(question, vocab)
    except Exception:
        return None
    if not isinstance(pat, dict):
        return None
    # The fake will name an undeclared predicate on purpose, and a real backend
    # can too. Checked here rather than trusted, because an undeclared
    # predicate does not fail — it matches nothing, which reads downstream as
    # "the corpus has no answer" when the truth is that the question was never
    # asked properly.
    spec = {p.name: p for p in vocab}.get(pat.get("p"))
    if spec is None or not isinstance(pat.get("s"), str) or not pat["s"].strip():
        return None
    return pat


def ask(tools, question: str, vocab, config, extractor,
        top_k: int | None = None) -> dict:
    """Answer a question symbolically if it can be, by retrieval otherwise.

    The result is an ordinary search result with `symbolic` saying which path
    produced it, and — on the symbolic path — the grounded `pattern` that did,
    so a caller can see what was actually asked rather than inferring it.
    """
    pat = formulate(question, vocab, config, extractor)
    if pat is None:
        return _retrieval(tools, question, top_k, config)

    spec = {p.name: p for p in vocab}[pat["p"]]
    try:
        subject_id = resolve(tools, pat["s"], config)
    except Exception:
        return _retrieval(tools, question, top_k, config)
    if subject_id is None:
        return _retrieval(tools, question, top_k, config)

    query = {"s": subject_id, "p": pat["p"]}
    if "o" in pat:
        if spec.object == "id":
            try:
                object_id = resolve(tools, pat["o"], config)
            except Exception:
                return _retrieval(tools, question, top_k, config)
            if object_id is None:
                # Not broadened by dropping `o`. That would answer a wider
                # question than the one asked and present the result as the
                # answer to this one, which is the silent substitution the
                # fallback exists to avoid.
                return _retrieval(tools, question, top_k, config)
            query["o"] = {"id": object_id}
        else:
            query["o"] = pat["o"]

    try:
        # `subsume` broadens the subject through `is_a` (5.3): a question about
        # a layer has to reach a fact about one of its components, which is the
        # entire multi-hop result. Derivations come back because a conclusion
        # nobody can check is the thing 5.3 was built not to produce.
        res = tools.search(pattern=query, subsume=True, derivations=True,
                           top_k=top_k)
    except Exception:
        return _retrieval(tools, question, top_k, config)
    if not res.get("ok") or not res.get("memories"):
        # An empty pattern result is not evidence of absence: the subject may
        # be fragmented across two entity records, or the fact may be asserted
        # in prose nobody has typed yet.
        return _retrieval(tools, question, top_k, config)

    res["symbolic"] = True
    res["pattern"] = query
    return res


def _premises_of(tools, route) -> list:
    """`(text, live)` for each premise of one route, prose fetched by id.

    A premise whose record cannot be read is kept with its id as the text
    rather than dropped: a proof with a step missing reads as a shorter proof,
    which is exactly the misreading a verbalization must not cause.
    """
    out = []
    for p in (route.get("premises") or [])[:_MAX_PREMISES_READ]:
        pid, live = p.get("id"), bool(p.get("live"))
        text = None
        if isinstance(pid, int):
            try:
                got = tools.get(pid)
            except Exception:
                got = {}
            if got.get("ok"):
                text = (got.get("memory") or {}).get("text")
        out.append(((text or f"record {pid}").strip(), live))
    return out


def verbalize(tools, memory: dict, config, extractor) -> str | None:
    """Prose for one memory's derivation, or None. Never raises.

    Reads the *shallowest* route, which is the one `depth` reports and the
    shortest true answer to "why". The others are equally valid proofs; showing
    all of them would turn one sentence into a disjunction nobody asked for,
    and the payload is still there for a caller that wants them.
    """
    if not getattr(config, "ask_verbalize", False):
        return None
    deriv = memory.get("derivation")
    if not isinstance(deriv, dict):
        return None
    routes = [r for r in (deriv.get("routes") or []) if isinstance(r, dict)]
    if not routes:
        return None
    route = min(routes, key=lambda r: r.get("depth") or 0)
    premises = _premises_of(tools, route)
    if not premises:
        return None
    try:
        # Everything crossing this call is derived: two strings and a list
        # built here from `get` results. Nothing the provider receives aliases
        # the record's payload, so "the model cannot alter the derivation" is a
        # property of what is passed rather than a convention about how
        # backends behave. Handing `route` over directly would be the natural
        # simplification and would quietly end that; test_ask pins it.
        return extractor.verbalize(str(memory.get("text") or ""),
                                   str(route.get("rule") or "?"), premises)
    except Exception:
        return None


def verbalize_all(tools, res: dict, config, extractor) -> dict:
    """Attach `because` to every memory in a result that carries a derivation.

    Beside `derivation`, never instead of it. A reader who wants the sentence
    gets the sentence; a reader who wants to check it gets the rule, the
    premises and whether each is still live, unchanged.
    """
    if not getattr(config, "ask_verbalize", False):
        return res
    for mem in res.get("memories") or []:
        prose = verbalize(tools, mem, config, extractor)
        if prose:
            mem["because"] = prose
    return res
