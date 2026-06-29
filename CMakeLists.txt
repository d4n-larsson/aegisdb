cmake_minimum_required(VERSION 3.20)
project(aegisdb LANGUAGES C VERSION 0.1.0)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # POSIX features (sockets, pthreads)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Opt-in host-CPU tuning (mirrors the Makefile's NATIVE=1): auto-vectorizes the
# vector-search hot loops for a faster semantic search, but the binary uses
# instructions specific to this machine and is NOT portable to other CPUs.
# Off by default; enable with -DAEGISDB_NATIVE=ON.
option(AEGISDB_NATIVE "Optimize for the host CPU (-march=native); not portable" OFF)
if(AEGISDB_NATIVE)
  add_compile_options(-march=native)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Vendored cJSON (T003)
# ---------------------------------------------------------------------------
add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)

# ---------------------------------------------------------------------------
# Core library: all source except the main entry point
# ---------------------------------------------------------------------------
file(GLOB_RECURSE AEGISDB_CORE_SOURCES CONFIGURE_DEPENDS src/*.c)
list(REMOVE_ITEM AEGISDB_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c)

add_library(aegisdb_core STATIC ${AEGISDB_CORE_SOURCES})
target_include_directories(aegisdb_core PUBLIC include third_party/cjson)
target_link_libraries(aegisdb_core PUBLIC cjson Threads::Threads m)

# Relaxed FP only for the vector-search translation unit, so the cosine
# reduction can vectorize under AEGISDB_NATIVE; scoped here so isnan/isinf
# elsewhere (e.g. cJSON number parsing) keep strict IEEE semantics.
if(AEGISDB_NATIVE)
  set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/semantic_index.c
    PROPERTIES COMPILE_OPTIONS "-ffast-math")
endif()

# Version baked into the binary. Override with -DAEGISDB_VERSION=x.y.z; otherwise
# derive from `git describe` (stripping a leading v), or fall back to a dev marker.
if(NOT DEFINED AEGISDB_VERSION)
  execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE _git_ver OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_git_ver)
    string(REGEX REPLACE "^v" "" AEGISDB_VERSION "${_git_ver}")
  else()
    set(AEGISDB_VERSION "0.0.0-dev")
  endif()
endif()
target_compile_definitions(aegisdb_core PUBLIC AEGIS_VERSION_STRING="${AEGISDB_VERSION}")

# ---------------------------------------------------------------------------
# Server executable
# ---------------------------------------------------------------------------
add_executable(aegisdb src/main.c)
target_link_libraries(aegisdb PRIVATE aegisdb_core)

# ---------------------------------------------------------------------------
# Tests (T004)
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests/unit)

# Wire-protocol contract tests (drive the server over TCP). Registered only
# when a Python interpreter is available; the test launches the built binary.
find_program(PYTHON3_EXECUTABLE NAMES python3 python)
if(PYTHON3_EXECUTABLE)
  add_test(
    NAME wire_protocol_contract
    COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/test_wire_protocol.py
            $<TARGET_FILE:aegisdb>)
endif()