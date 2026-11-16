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
scada_test_command(sim_command scada-sim)
scada_add_test(scada-tag-wake-perf-test KIND perf
  SOURCES tag_wake_perf_test.cpp
  DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_SIM_COMMAND=${sim_command}")
target_include_directories(scada-tag-wake-perf-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../integration")
add_dependencies(scada-tag-wake-perf-test scada-tagd scada-sim)

# The rate of the simulator writer (issue P1-10): 50,000 updates per second
# across 10 owners, as threads of one process and as 10 processes.
scada_add_test(scada-sim-rate-perf-test KIND perf
  SOURCES sim_rate_perf_test.cpp
  DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_SIM_COMMAND=${sim_command}")
target_include_directories(scada-sim-rate-perf-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../integration")
add_dependencies(scada-sim-rate-perf-test scada-tagd scada-sim)

# The post of a queue with the high-water mark of the base sequence beside
# a cursor another producer reads (issue #53, condition C9): two contexts,
# four threads, one million records each way.
scada_add_test(scada-tag-queue-post-perf-test KIND perf
  SOURCES queue_post_perf_test.cpp
  DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}")
target_include_directories(scada-tag-queue-post-perf-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../integration")
add_dependencies(scada-tag-queue-post-perf-test scada-tagd)
