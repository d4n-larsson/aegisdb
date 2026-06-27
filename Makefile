# Fallback build for environments without CMake.
# The canonical build is CMake (see CMakeLists.txt); this Makefile mirrors it
# so the project can be compiled and tested with plain make.

CC      ?= cc
CSTD    := -std=c17
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS:= -Iinclude -Ithird_party/cjson -D_GNU_SOURCE
LDFLAGS :=
LDLIBS  := -lpthread -lm

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