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

# Conformance test 27: the latency of read, read_many, write and write_many
# under the reference load, the scan of 5,000 tags, the memory of each
# process and the size of the database (issue P1-12, decision D-57). The
# medians are compared with the baseline of the architecture the tests run
# on, in baseline/, if it names the call.
scada_add_test(scada-tag-latency-perf-test KIND perf
  SOURCES tag_latency_perf_test.cpp
  DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_SIM_COMMAND=${sim_command}"
    "SCADA_PERF_BASELINE=${CMAKE_CURRENT_SOURCE_DIR}/baseline/${CMAKE_SYSTEM_PROCESSOR}.txt")
target_include_directories(scada-tag-latency-perf-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../integration")
add_dependencies(scada-tag-latency-perf-test scada-tagd scada-sim)

# The gap half of contract 16, "default change ring capacity": a consumer
# that keeps up with the reference load gets no gap (issue P1-13, decision
# D-64). 10 s here; the workflow `measurements` runs it for 600 s.
scada_add_test(scada-tag-ring-gap-perf-test KIND perf
  SOURCES ring_gap_perf_test.cpp
  DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_SIM_COMMAND=${sim_command}")
target_include_directories(scada-tag-ring-gap-perf-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../integration")
add_dependencies(scada-tag-ring-gap-perf-test scada-tagd scada-sim)

# Huge pages for the `tags` regions (contract 16): read latency with the
# database on a tmpfs mounted huge=never and huge=always (issue P1-13,
# decision D-64). It mounts a file system, so its label is `root` instead
# of `perf`: `sudo ctest --preset root` runs it, and the workflow
# `measurements` runs it in the `perf` build.
scada_add_test(scada-tag-huge-pages-root-test KIND perf
  SOURCES huge_pages_root_perf_test.cpp
  DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_SIM_COMMAND=${sim_command}")
set_tests_properties(scada-tag-huge-pages-root-test PROPERTIES LABELS root)
target_include_directories(scada-tag-huge-pages-root-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../integration")
add_dependencies(scada-tag-huge-pages-root-test scada-tagd scada-sim)
