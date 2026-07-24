# Fallback build for environments without CMake.
# The canonical build is CMake (see CMakeLists.txt); this Makefile mirrors it
# so the project can be compiled and tested with plain make.

CC      ?= cc
CSTD    := -std=c17
CFLAGS  ?= -O2 -g -Wall -Wextra
# Treat warnings as errors — off by default so a downstream build on a newer
# compiler (which may introduce new warnings) isn't broken by them; CI turns it
# on with `make ... WERROR=1` to keep the tree warning-clean.
WERROR  ?= 0
ifeq ($(WERROR),1)
CFLAGS += -Werror
endif
# Version baked into the binary: `git describe` (tag-derived) by default,
# overridable with `make VERSION=x.y.z`.
GIT_VERSION := $(shell git describe --tags --always --dirty 2>/dev/null | sed 's/^v//')
VERSION ?= $(or $(GIT_VERSION),0.0.0-dev)
CPPFLAGS:= -Iinclude -Ithird_party/cjson -D_GNU_SOURCE -DAEGIS_VERSION_STRING='"$(VERSION)"'
# Auto-track header dependencies: -MMD emits a .d per compile listing the headers
# it included, -MP adds phony targets so a deleted header doesn't break the build.
# Included below, so editing a header in include/ rebuilds every object that uses
# it — closing the stale-object footgun (previously `make clean` was required
# after any header edit; the canonical CMake build already tracks this).
CPPFLAGS += -MMD -MP
# `?=` so a sanitizer/CI build can pass LDFLAGS via the environment (the server
# link rule uses only LDFLAGS, unlike the test rule which also uses CFLAGS).
LDFLAGS ?=
LDLIBS  := -lpthread -lm

# Opt-in host-CPU tuning (`make NATIVE=1`): -O3 + -march=native auto-vectorizes
# the vector-search hot loops for a faster semantic search, but the resulting
# binary uses instructions specific to this machine and is NOT portable to other
# CPUs. Off by default so the standard build stays portable. (-O3 matters: at
# the default -O2 the cosine reduction stays scalar. The canonical CMake build
# is already Release/-O3.)
ifeq ($(NATIVE),1)
CFLAGS += -O3 -march=native
endif

BUILD   := build
CORE_SRC := $(filter-out src/main.c,$(shell find src -name '*.c'))
CORE_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRC))
CJSON_OBJ := $(BUILD)/third_party/cjson/cJSON.o
MAIN_OBJ := $(BUILD)/src/main.o

BIN := $(BUILD)/aegisdb

# Unity test framework
UNITY_OBJ := $(BUILD)/third_party/unity/unity.o
TEST_SRC  := $(wildcard tests/unit/*.c)
TEST_BIN  := $(patsubst tests/unit/%.c,$(BUILD)/tests/%,$(TEST_SRC))

# The .d files emitted by -MMD (one per object/binary); `-include`d at the very
# bottom of this file so the auto-generated .o rules they carry can't shadow
# `all` as the default goal.
DEPS := $(CORE_OBJ:.o=.d) $(MAIN_OBJ:.o=.d) $(CJSON_OBJ:.o=.d) \
        $(UNITY_OBJ:.o=.d) $(TEST_BIN:=.d)

PYTHON ?= python3

.PHONY: all clean test integration check eval eval-tasks inspector inspector-test bench wire-bench fuzz fuzz-regress fuzz-corpus
all: $(BIN)

$(BIN): $(CORE_OBJ) $(CJSON_OBJ) $(MAIN_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# Relaxed FP only for the vector-search translation units, so the cosine
# reduction can vectorize under NATIVE; scoped here so isnan/isinf elsewhere
# (e.g. cJSON number parsing) keep strict IEEE semantics. Both the exact scan
# (semantic_index) and the HNSW graph distance (hnsw) are hot dot-product loops.
ifeq ($(NATIVE),1)
$(BUILD)/src/storage/semantic_index.o: CFLAGS += -ffast-math
$(BUILD)/src/storage/hnsw.o: CFLAGS += -ffast-math
endif

# Tests: each test file links against all core objects + unity.
$(BUILD)/tests/%: tests/unit/%.c $(CORE_OBJ) $(CJSON_OBJ) $(UNITY_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CFLAGS) $(CPPFLAGS) -Ithird_party/unity -o $@ $< $(CORE_OBJ) $(CJSON_OBJ) $(UNITY_OBJ) $(LDLIBS)

test: $(TEST_BIN)
	@fail=0; for t in $(TEST_BIN); do echo "== $$t =="; $$t || fail=1; done; exit $$fail

# Wire-protocol contract tests: drive the running server over TCP.
integration: $(BIN)
	$(PYTHON) tests/contract/test_wire_protocol.py $(BIN)

# Recall-quality eval: seed a labelled corpus, run queries, report recall@k/MRR
# (ROADMAP Horizon 1.1). Report-only by default; add EVAL_ARGS='--gate-recall-at
# 5 --gate-threshold 0.8' to fail on a regression. Uses the deterministic hashing
# embedder so it needs no model/API.
eval: $(BIN)
	$(PYTHON) eval/recall_eval.py $(BIN) $(EVAL_ARGS)

# A/B task benchmark: does memory lift task success? Teaches a fact, then answers
# a fresh question with memory ON vs OFF and reports the lift (ROADMAP 1.1+).
# Default `fake` answer model runs in CI; point at a real one for a real number:
# EVAL_ARGS='--model claude-code' (or --model anthropic --judge).
eval-tasks: $(BIN)
	$(PYTHON) eval/ab_tasks.py $(BIN) $(EVAL_ARGS)

# Memory-inspection UI (ROADMAP 1.3): a local HTTP bridge that proxies to a
# running aegisdb and serves the browser inspector. Point it at your server with
# INSPECTOR_ARGS='--aegis-port 9470 --token <tok> --embedding-dim <dim>'.
inspector:
	$(PYTHON) tools/inspector/bridge.py $(INSPECTOR_ARGS)

# Smoke test for the bridge: spawns a server + bridge and exercises every UI
# endpoint (config/stats/browse/embed/explain/edit/delete + the op allowlist).
inspector-test: $(BIN)
	$(PYTHON) tools/inspector/test_bridge.py $(BIN)

# Line-coverage report (gcov; no lcov/gcovr needed). Rebuilds everything
# instrumented, runs the unit tests AND the contract suite, and aggregates.
# AEGIS_COV=1 makes the contract harness shut servers down gracefully (SIGTERM)
# so gcov flushes their data — SIGKILL (the normal fast path) would drop it.
# The unit-test and server binaries share the same instrumented core objects, so
# .gcda counts accumulate across both into a combined figure.
COV_CFLAGS := -O0 -g -fprofile-arcs -ftest-coverage -Wall -Wextra
.PHONY: coverage
coverage:
	$(MAKE) clean
	CFLAGS="$(COV_CFLAGS)" LDFLAGS="-fprofile-arcs -ftest-coverage" $(MAKE) -k test || true
	AEGIS_COV=1 CFLAGS="$(COV_CFLAGS)" LDFLAGS="-fprofile-arcs -ftest-coverage" \
	  $(MAKE) integration || true
	$(PYTHON) scripts/coverage.py

# HNSW recall/latency benchmark (not part of `test` — too heavy for CI). Runs a
# default config; pass args to the binary for other dims/sizes, e.g.
#   ./build/bench/hnsw_bench 1024 50000 200
# Add NATIVE=1 for vectorized (representative) latency numbers.
# hnsw.o pulls in ckpt_crypt (checkpoint encryption), which pulls in the AEAD.
HNSW_BENCH_OBJ := $(BUILD)/src/storage/hnsw.o $(BUILD)/src/util/crc32.o \
                  $(BUILD)/src/storage/ckpt_crypt.o \
                  $(BUILD)/src/util/chacha20poly1305.o
$(BUILD)/bench/hnsw_bench: bench/hnsw_bench.c $(HNSW_BENCH_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(HNSW_BENCH_OBJ) $(LDLIBS)
bench: $(BUILD)/bench/hnsw_bench
	$(BUILD)/bench/hnsw_bench

# End-to-end wire-protocol benchmark: drives a RUNNING server over TCP and
# reports throughput + latency percentiles for the full pipeline. Build with
# `make wire-bench` (or `make NATIVE=1 wire-bench`), then run against a server:
#   ./build/aegisdb --data-dir /tmp/wb --port 9470 &
#   ./build/bench/wire_bench --op mixed --conns 16 --secs 10
$(BUILD)/bench/wire_bench: bench/wire_bench.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDLIBS)
wire-bench: $(BUILD)/bench/wire_bench

# ---- Fuzzing (tests/fuzz) --------------------------------------------------
# Coverage-guided fuzzing of the two attacker-reachable parse surfaces: the
# binary log record codec (record_decode) and the wire request path
# (aegis_request_handle). Two build flavors share the target sources:
#   `make fuzz`         libFuzzer + ASan/UBSan binaries for local/nightly runs
#                       (needs clang; libFuzzer supplies main()).
#   `make fuzz-regress` deterministic replay of the seed corpus + checked-in
#                       crashers via standalone_main.c — no libFuzzer, so it
#                       builds under the default $(CC) (gcc-OK) and is the fast
#                       per-PR regression gate.
#   `make fuzz-corpus`  (re)generate the seed corpus under tests/fuzz/corpus.
CORPUS := tests/fuzz/corpus

# libFuzzer flavor: instrument every core object (fuzzer-no-link) and link the
# target with -fsanitize=fuzzer so libFuzzer drives it.
FUZZ_CC     ?= clang
FUZZ_SAN    ?= -fsanitize=fuzzer-no-link,address,undefined
FUZZ_CFLAGS := -O1 -g -fno-omit-frame-pointer $(FUZZ_SAN) -Wall -Wextra
FUZZ_OBJDIR := $(BUILD)/fuzz-obj
FUZZ_CORE_OBJ  := $(patsubst %.c,$(FUZZ_OBJDIR)/%.o,$(CORE_SRC))
FUZZ_CJSON_OBJ := $(FUZZ_OBJDIR)/third_party/cjson/cJSON.o
FUZZ_TARGETS   := $(BUILD)/fuzz/fuzz_record_decode $(BUILD)/fuzz/fuzz_wire

$(FUZZ_OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(CSTD) $(FUZZ_CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILD)/fuzz/fuzz_%: tests/fuzz/fuzz_%.c $(FUZZ_CORE_OBJ) $(FUZZ_CJSON_OBJ)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(CSTD) $(FUZZ_CFLAGS) -fsanitize=fuzzer $(CPPFLAGS) -o $@ $< \
	  $(FUZZ_CORE_OBJ) $(FUZZ_CJSON_OBJ) $(LDLIBS)

fuzz: $(FUZZ_TARGETS)

# Standalone-replay flavor: target source + standalone_main.c, linked against
# the ordinary core objects (so it inherits whatever CFLAGS the caller set —
# e.g. the ASan job's sanitizers — and builds under gcc).
$(BUILD)/fuzz/regress_%: tests/fuzz/fuzz_%.c tests/fuzz/standalone_main.c $(CORE_OBJ) $(CJSON_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CFLAGS) $(CPPFLAGS) -o $@ tests/fuzz/fuzz_$*.c \
	  tests/fuzz/standalone_main.c $(CORE_OBJ) $(CJSON_OBJ) $(LDLIBS)

$(BUILD)/fuzz/gen_seeds: tests/fuzz/gen_seeds.c $(CORE_OBJ) $(CJSON_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(CORE_OBJ) $(CJSON_OBJ) $(LDLIBS)

fuzz-corpus: $(BUILD)/fuzz/gen_seeds
	@mkdir -p $(CORPUS)/record $(CORPUS)/wire
	$(BUILD)/fuzz/gen_seeds $(CORPUS)/record $(CORPUS)/wire

# Replay every seed + crasher through the standalone drivers. A crash (or
# sanitizer abort) fails the target; an empty arg list is a clean no-op. This is
# what turns a libFuzzer find (minimized into corpus/crashers/) into a permanent
# regression test, and what the per-PR CI gate runs.
fuzz-regress: $(BUILD)/fuzz/regress_record_decode $(BUILD)/fuzz/regress_wire fuzz-corpus
	@rec=$$(ls $(CORPUS)/record/* $(CORPUS)/crashers/record/* 2>/dev/null); \
	 wir=$$(ls $(CORPUS)/wire/* $(CORPUS)/crashers/wire/* 2>/dev/null); \
	 $(BUILD)/fuzz/regress_record_decode $$rec && \
	 $(BUILD)/fuzz/regress_wire $$wir && \
	 echo "fuzz-regress: corpus replayed cleanly"

DEPS += $(FUZZ_CORE_OBJ:.o=.d) $(FUZZ_CJSON_OBJ:.o=.d)

# Run the full suite: C unit tests + protocol contract tests.
check: test integration

clean:
	rm -rf $(BUILD)

# Header-dependency files, included last so their generated .o rules only add
# prerequisites and never become the default goal. Leading '-' ignores them on a
# clean tree; `clean` removes them with build/.
-include $(DEPS)