# Performance tests. They are registered in every build so that they keep
# compiling, but only `ctest --preset perf` runs them: the figures mean
# something only with the release flags.
scada_add_test(scada-hello-perf-test KIND perf
  SOURCES hello_perf_test.cpp
  DEPENDS scada::hello)

scada_add_test(scada-tag-header-perf-test KIND perf
  SOURCES tag_header_perf_test.cpp
  DEPENDS scada::tag)

# The wake-up of a blocked change ring consumer (issue P1-07): a writer
# process and this one. It uses the fixture of the integration tests, and
# scada_test_command from their CMakeLists.txt.
scada_test_command(tagd_command scada-tagd)
scada_test_command(writer_command scada-tag-test-writer)
scada_add_test(scada-tag-wake-perf-test KIND perf
  SOURCES tag_wake_perf_test.cpp
  DEPENDS scada::tag scada::tagd scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_TAG_WRITER_COMMAND=${writer_command}")
target_include_directories(scada-tag-wake-perf-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../integration")
add_dependencies(scada-tag-wake-perf-test scada-tagd scada-tag-test-writer)
