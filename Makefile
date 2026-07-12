# Fallback build for environments without CMake.
# The canonical build is CMake (see CMakeLists.txt); this Makefile mirrors it
# so the project can be compiled and tested with plain make.

CC      ?= cc
CSTD    := -std=c17
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
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

.PHONY: all clean test integration check bench wire-bench
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

# Line-coverage report (gcov; no lcov/gcovr needed). Rebuilds everything
# instrumented, runs the unit tests AND the contract suite, and aggregates.
# AEGIS_COV=1 makes the contract harness shut servers down gracefully (SIGTERM)
# so gcov flushes their data — SIGKILL (the normal fast path) would drop it.
# The unit-test and server binaries share the same instrumented core objects, so
# .gcda counts accumulate across both into a combined figure.
COV_CFLAGS := -O0 -g -fprofile-arcs -ftest-coverage -Wall -Wextra -Wno-unused-parameter
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

# Run the full suite: C unit tests + protocol contract tests.
check: test integration

clean:
	rm -rf $(BUILD)

# Header-dependency files, included last so their generated .o rules only add
# prerequisites and never become the default goal. Leading '-' ignores them on a
# clean tree; `clean` removes them with build/.
-include $(DEPS)