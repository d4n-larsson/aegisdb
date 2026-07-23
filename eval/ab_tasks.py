#!/usr/bin/env python3
"""A/B task benchmark: does AegisDB memory actually help an agent? (ROADMAP 1.1+)

`make eval` measures *retrieval* quality (did the right memory rank?). This
measures *task outcomes*: for each task we teach a fact in one "session" (store it
in AegisDB), then answer a question in a fresh session two ways —

  ON  : recall relevant memories and inject them into the prompt
  OFF : answer with no memory (the baseline)

— and report the success rate of each and the LIFT (ON − OFF). If memory doesn't
lift task success, it isn't earning its tokens; if it does, that's the core
"is this useful?" number.

The answer model is a seam (fake | claude-code | anthropic | openai). The default
`fake` model answers only from injected context, so ON succeeds and OFF fails —
demonstrating the harness isolates the memory effect. Point it at a real backend
for a real number:

    make eval-tasks EVAL_ARGS='--model claude-code'
    python3 eval/ab_tasks.py ./build/aegisdb --model anthropic --judge

Usage:
    python3 eval/ab_tasks.py [path/to/aegisdb] [--dataset ...] [--model fake]
        [--embedder subword] [--top-k 5] [--judge] [--min-lift 0.5] [--json]
"""
from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embedders import resolve_embedder  # noqa: E402
from models import resolve_model  # noqa: E402
from recall_eval import Server  # noqa: E402  (reuse the spawn + wire client)

_ANSWER_INSTR = "Answer in one short sentence. If you don't know, say you don't know."


def build_prompt(question, memories=None):
    if memories:
        ctx = "\n".join(f"- {m}" for m in memories)
        return f"Context:\n{ctx}\n\nQuestion: {question}\n{_ANSWER_INSTR}\nAnswer:"
    return f"Question: {question}\n{_ANSWER_INSTR}\nAnswer:"


def grade(answer, task, model, judge):
    """True if the answer is correct. Keyword match by default; --judge asks the
    model for a yes/no when a task has no expect_any (or to be stricter)."""
    a = (answer or "").lower()
    exp = task.get("expect_any")
    if exp and not judge:
        return any(k.lower() in a for k in exp)
    if judge:
        rubric = task.get("rubric") or ("it states: " + "; ".join(task.get("expect_any", [])))
        # Grade factual match, not appropriateness — otherwise the judge rubber-
        # stamps "I don't know" as an acceptable answer (it does, by default).
        v = model.answer(
            "You grade whether an answer states a specific required fact.\n"
            f"Question: {task['question']}\n"
            f"Answer to grade: {answer}\n"
            f"The answer is CORRECT only if {rubric}. If it says it doesn't know, "
            "is unsure, or omits that fact, it is INCORRECT.\n"
            "Reply with exactly one word: YES or NO.")
        return v.strip().lower().startswith("yes")
    return bool(exp) and any(k.lower() in a for k in exp)


def recall(srv, agent_id, embed, question, top_k):
    r = srv.req({"operation": "search", "embedding": embed(question), "top_k": top_k,
                 "agent_id": agent_id, "include_embeddings": False})
    return [rec["data"] for rec in r.get("records", [])] if r.get("ok") else []


def run(args):
    with open(args.dataset) as fh:
        ds = json.load(fh)
    dim = ds.get("embedding_dim", 256)
    embed = resolve_embedder(args.embedder, dim, args.embedder_cmd)
    model = resolve_model(args.model, args.model_name, args.api_base, args.sandbox)
    if not model.available():
        print(f"model backend '{args.model}' unavailable "
              f"(missing SDK/key/CLI); nothing to measure", file=sys.stderr)
        return 2

    rows = []
    with Server(args.binary, args.port, dim) as srv:
        for t in ds["tasks"]:
            ns = f"ab-{t['id']}"
            # teach: store the task's memories in this task's own namespace
            for mem in t["memories"]:
                srv.req({"operation": "insert", "type": "semantic", "data": mem,
                         "agent_id": ns, "embedding": embed(mem),
                         "include_embeddings": False})
            # OFF: no memory
            off = model.answer(build_prompt(t["question"]))
            off_ok = grade(off, t, model, args.judge)
            # ON: recall + inject
            mems = recall(srv, ns, embed, t["question"], args.top_k)
            on = model.answer(build_prompt(t["question"], mems))
            on_ok = grade(on, t, model, args.judge)
            rows.append({"id": t["id"], "question": t["question"],
                         "on": on_ok, "off": off_ok,
                         "on_answer": on, "off_answer": off, "recalled": len(mems)})

    n = len(rows) or 1
    on_rate = sum(r["on"] for r in rows) / n
    off_rate = sum(r["off"] for r in rows) / n
    lift = on_rate - off_rate

    if args.json:
        print(json.dumps({"dataset": ds["name"], "model": args.model, "n": len(rows),
                          "on_rate": on_rate, "off_rate": off_rate, "lift": lift,
                          "tasks": rows}, indent=2))
    else:
        print(f"\nAegisDB A/B task benchmark — '{ds['name']}', model '{args.model}', "
              f"{len(rows)} tasks\n")
        print(f"  {'ON':>3} {'OFF':>4}  task")
        print(f"  {'--':>3} {'---':>4}  {'-'*48}")
        for r in rows:
            mark = lambda b: " ✓ " if b else " · "
            print(f"  {mark(r['on'])}{mark(r['off'])} {r['question'][:52]}")
        print(f"\n  with memory (ON):    {on_rate:.0%}")
        print(f"  without memory (OFF): {off_rate:.0%}")
        print(f"  lift:                +{lift:.0%}  (ON − OFF)\n")

    if args.min_lift is not None and lift < args.min_lift:
        print(f"GATE FAILED: lift {lift:.0%} < required {args.min_lift:.0%}", file=sys.stderr)
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description="AegisDB A/B task benchmark (memory ON vs OFF)")
    ap.add_argument("binary", nargs="?", default="./build/aegisdb")
    ap.add_argument("--dataset", default="eval/datasets/ab_tasks.json")
    ap.add_argument("--embedder", default="subword", choices=["subword", "hashing", "command"])
    ap.add_argument("--embedder-cmd", default=None)
    ap.add_argument("--model", default="fake", choices=["fake", "claude-code", "anthropic", "openai"])
    ap.add_argument("--model-name", default="", help="model id override for the backend")
    ap.add_argument("--api-base", default="", help="openai backend base URL")
    ap.add_argument("--port", type=int, default=9972)
    ap.add_argument("--top-k", type=int, default=5, help="memories recalled for the ON arm")
    ap.add_argument("--judge", action="store_true", help="grade with the model (LLM-as-judge)")
    ap.add_argument("--sandbox", action="store_true",
                    help="claude-code: run from an empty dir with tools disabled, so the "
                         "OFF arm has no filesystem/memory side channel (a clean baseline)")
    ap.add_argument("--min-lift", type=float, default=None, help="fail if ON−OFF < this (0..1)")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
