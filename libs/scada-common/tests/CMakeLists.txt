# These tests use no test framework: the framework is chosen in issue P0-03,
# and they move to it then (as libscada-hello's test does).
foreach(name IN ITEMS status result clock fd log log_sink)
  string(REPLACE "_" "-" dashed "${name}")
  set(test "scada-common-${dashed}-unit-test")
  scada_add_test(${test} SOURCES ${name}_test.cpp DEPENDS scada::common)
  # log_internal.h: helpers that are not part of the public interface.
  target_include_directories(${test}
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
  set_tests_properties(${test} PROPERTIES TIMEOUT 120)
endforeach()
