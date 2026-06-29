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
LDFLAGS :=
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

PYTHON ?= python3

.PHONY: all clean test integration check
all: $(BIN)

$(BIN): $(CORE_OBJ) $(CJSON_OBJ) $(MAIN_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# Relaxed FP only for the vector-search translation unit, so the cosine
# reduction can vectorize under NATIVE; scoped here so isnan/isinf elsewhere
# (e.g. cJSON number parsing) keep strict IEEE semantics.
ifeq ($(NATIVE),1)
$(BUILD)/src/storage/semantic_index.o: CFLAGS += -ffast-math
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

# Run the full suite: C unit tests + protocol contract tests.
check: test integration

clean:
	rm -rf $(BUILD)