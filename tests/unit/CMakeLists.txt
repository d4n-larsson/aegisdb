# Unit test scaffold (T004)
#
# Tests were not explicitly requested for this feature (see tasks.md), so no
# Unity test sources are checked in yet. This scaffold registers the vendored
# Unity framework and auto-discovers any tests/unit/*.c files added later,
# building one CTest target per file. It is a no-op when no test files exist.

add_library(unity STATIC ${CMAKE_SOURCE_DIR}/third_party/unity/unity.c)
target_include_directories(unity PUBLIC ${CMAKE_SOURCE_DIR}/third_party/unity)

file(GLOB UNIT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

foreach(test_src ${UNIT_TEST_SOURCES})
  get_filename_component(test_name "${test_src}" NAME_WE)
  add_executable(${test_name} "${test_src}")
  target_link_libraries(${test_name} PRIVATE aegisdb_core unity)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()