# Integration tests: each starts real processes.

# The command that starts the executable <target>, elements separated by
# `|`. A cross build runs its programs through the emulator, and so must the
# test.
function(scada_test_command variable target)
  set(command ${CMAKE_CROSSCOMPILING_EMULATOR} "$<TARGET_FILE:${target}>")
  list(JOIN command "|" command)
  set(${variable} "${command}" PARENT_SCOPE)
endfunction()

scada_test_command(hello_command scada-hello)
scada_add_test(scada-hello-integration-test KIND integration
  SOURCES hello_integration_test.cpp
  DEPENDS scada::hello scada::service scada::test-support
  ENVIRONMENT "SCADA_HELLO_COMMAND=${hello_command}")

scada_test_command(tagd_command scada-tagd)
scada_test_command(peer_command scada-tag-test-peer)
scada_add_test(scada-tagd-integration-test KIND integration
  SOURCES tagd_integration_test.cpp
  DEPENDS scada::tagd scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_TAG_PEER_COMMAND=${peer_command}")
add_dependencies(scada-tagd-integration-test scada-tagd scada-tag-test-peer)

# Tag read and write with real processes: conformance tests 2, 3, 5 and 6 of
# the tag database contract, the part of the reader. scada-sim writes the
# load.
scada_test_command(writer_command scada-tag-test-writer)
scada_test_command(sim_command scada-sim)
# The library checks the thread of an owner (contract 13.2) only in a build
# without NDEBUG; CMake defines NDEBUG in every build type but Debug.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(thread_check 1)
else()
  set(thread_check 0)
endif()
scada_add_test(scada-tag-access-integration-test KIND integration
  SOURCES tag_access_integration_test.cpp
  DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_TAG_PEER_COMMAND=${peer_command}"
    "SCADA_TAG_WRITER_COMMAND=${writer_command}"
    "SCADA_SIM_COMMAND=${sim_command}"
    "SCADA_TAG_THREAD_CHECK=${thread_check}")
add_dependencies(scada-tag-access-integration-test
  scada-tagd scada-tag-test-peer scada-tag-test-writer scada-sim)
# Conformance test 3 at the maximum rate, with writer and readers on
# separate processors, is a CTest test of its own, so that it shows as
# skipped, not passed, on a machine with one processor (decision D-57). The
# test above leaves it out. A command-line filter takes precedence over
# GTEST_FILTER in the environment.
set(separate_cores_case
  "TagAccessProcesses.Conformance03MaximumWriteRateWithFourReaders")
get_test_property(scada-tag-access-integration-test ENVIRONMENT access_env)
add_test(NAME scada-tag-separate-cores-integration-test
  COMMAND scada-tag-access-integration-test
    "--gtest_filter=${separate_cores_case}")
set_tests_properties(scada-tag-separate-cores-integration-test PROPERTIES
  LABELS integration
  TIMEOUT 300
  ENVIRONMENT "${access_env}"
  SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")
set_property(TEST scada-tag-access-integration-test APPEND PROPERTY
  ENVIRONMENT "GTEST_FILTER=-${separate_cores_case}")

# The change rings with real processes: conformance tests 7 (the ring), 13
# and 24.
scada_add_test(scada-tag-change-ring-integration-test KIND integration
  SOURCES change_ring_integration_test.cpp
  DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_TAG_PEER_COMMAND=${peer_command}"
    "SCADA_SIM_COMMAND=${sim_command}")
add_dependencies(scada-tag-change-ring-integration-test
  scada-tagd scada-tag-test-peer scada-sim)

# scada-sim itself with real processes: the tag-set command, every pattern,
# several simulator processes, the stop on a signal, and the errors.
scada_add_test(scada-sim-integration-test KIND integration
  SOURCES sim_integration_test.cpp
  DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_TAG_PEER_COMMAND=${peer_command}"
    "SCADA_SIM_COMMAND=${sim_command}")
add_dependencies(scada-sim-integration-test scada-tagd scada-sim)

# The reliable queues with real processes: conformance tests 16, 17 and
# 19, and one million records through one queue.
scada_test_command(queue_command scada-tag-test-queue)
scada_add_test(scada-tag-queue-integration-test KIND integration
  SOURCES queue_integration_test.cpp
  DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_TAG_QUEUE_COMMAND=${queue_command}"
    "SCADA_TAG_THREAD_CHECK=${thread_check}")
add_dependencies(scada-tag-queue-integration-test
  scada-tagd scada-tag-test-queue)

# Observer contexts with real processes (contract, section 11.4).
scada_add_test(scada-tag-observer-integration-test KIND integration
  SOURCES observer_integration_test.cpp
  DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_TAG_PEER_COMMAND=${peer_command}"
    "SCADA_SIM_COMMAND=${sim_command}")
add_dependencies(scada-tag-observer-integration-test
  scada-tagd scada-tag-test-peer scada-sim)

# scadactl against real processes (issue #19).
scada_test_command(scadactl_command scadactl)
scada_add_test(scadactl-integration-test KIND integration
  SOURCES scadactl_integration_test.cpp
  DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_TAG_PEER_COMMAND=${peer_command}"
    "SCADA_SIM_COMMAND=${sim_command}"
    "SCADACTL_COMMAND=${scadactl_command}")
add_dependencies(scadactl-integration-test
  scadactl scada-tagd scada-tag-test-peer scada-sim)

# Who may observe, with real users (contract, sections 3.1 and 11.4). It
# must run as root to become other users, so it has the label `root`
# instead of `integration`: the presets that run unprivileged leave it out,
# and `ctest --preset root` runs it (the pipeline does so with sudo).
scada_add_test(scada-tag-observer-users-test KIND integration
  SOURCES observer_root_integration_test.cpp
  DEPENDS scada::tag)
set_tests_properties(scada-tag-observer-users-test PROPERTIES LABELS root)

# Conformance test 11, second part, on a file system that is really full: a
# small tmpfs mounted in a mount namespace of the test. Mounting needs root,
# so it has the label `root`, as the test above.
scada_add_test(scada-tag-full-shm-root-test KIND integration
  SOURCES full_shm_root_integration_test.cpp
  DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_TAG_PEER_COMMAND=${peer_command}")
set_tests_properties(scada-tag-full-shm-root-test PROPERTIES LABELS root)
add_dependencies(scada-tag-full-shm-root-test scada-tagd scada-tag-test-peer)

# Conformance test 26, false trips: the reference load with real-time
# readers under SCHED_FIFO beside the owners. SCHED_FIFO needs root, so it
# has the label `root`. scada-tag-rt-reader runs the readers; it is not a
# CTest test and is not installed.
add_executable(scada-tag-rt-reader rt_reader_main.cpp)
target_link_libraries(scada-tag-rt-reader
  PRIVATE scada::tag scada::sim scada::test-support)
scada_target_defaults(scada-tag-rt-reader)
scada_test_command(rt_reader_command scada-tag-rt-reader)
# The criterion of contract section 16 for SCADA_E_BUSY is a property of the
# code that ships: an optimised build without a sanitizer gates on it, the
# other builds report the rate only (decision D-57).
set(busy_gate 0)
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$"
    AND SCADA_SANITIZER STREQUAL "")
  set(busy_gate 1)
endif()
scada_add_test(scada-tag-false-trip-root-test KIND integration
  SOURCES false_trip_root_integration_test.cpp
  DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_SIM_COMMAND=${sim_command}"
    "SCADA_TAG_RT_READER_COMMAND=${rt_reader_command}"
    "SCADA_TAG_BUSY_GATE=${busy_gate}")
set_tests_properties(scada-tag-false-trip-root-test PROPERTIES LABELS root)
add_dependencies(scada-tag-false-trip-root-test
  scada-tagd scada-sim scada-tag-rt-reader)

# Conformance test 22: the real-time safe functions make no system call and
# no allocation. The probe is traced as a child process. That shows nothing
# in a sanitizer build, whose runtime makes system calls of its own, and
# cannot work through an emulator, so the test exists only in a native
# build without a sanitizer.
if(NOT SCADA_SANITIZER AND NOT CMAKE_CROSSCOMPILING)
  scada_test_command(probe_command scada-tag-rt-probe)
  scada_add_test(scada-tag-rt-integration-test KIND integration
    SOURCES rt_safety_integration_test.cpp
    DEPENDS scada::tag scada::tagd scada::sim scada::service scada::test-support
    ENVIRONMENT
      "SCADA_TAGD_COMMAND=${tagd_command}"
      "SCADA_TAG_PEER_COMMAND=${peer_command}"
      "SCADA_TAG_RT_PROBE_COMMAND=${probe_command}"
      "SCADA_TAG_QUEUE_COMMAND=${queue_command}")
  add_dependencies(scada-tag-rt-integration-test
    scada-tagd scada-tag-test-peer scada-tag-rt-probe scada-tag-test-queue)
endif()

# Retained values and the generation change with real processes: the
# retained-value part of conformance test 21, test 25, and SIGKILL at random
# points (issue #17; decision D-45).
scada_test_command(retainer_command scada-tag-test-retainer)
scada_test_command(hung_writer_command scada-tagd-test-hung-writer)
scada_add_test(scada-tag-retain-integration-test KIND integration
  SOURCES retain_integration_test.cpp
  DEPENDS scada::tag scada::tagd scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_TAG_RETAINER_COMMAND=${retainer_command}"
    "SCADA_TAGD_HUNG_WRITER_COMMAND=${hung_writer_command}")
# retain_file.h is private to the library: the test reads the files as
# attach does.
target_include_directories(scada-tag-retain-integration-test
  PRIVATE "${PROJECT_SOURCE_DIR}/libs/scada-tag/src")
add_dependencies(scada-tag-retain-integration-test
  scada-tagd scada-tag-test-retainer scada-tagd-test-hung-writer)

# The queue part of conformance test 21: across a generation change no
# event a post accepted is lost and no request of the old generation is
# executed (issue #65; decision D-54).
scada_test_command(crosser_command scada-tag-test-crosser)
scada_add_test(scada-tag-retiring-queue-integration-test KIND integration
  SOURCES retiring_queue_integration_test.cpp
  DEPENDS scada::tag scada::tagd scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_TAG_CROSSER_COMMAND=${crosser_command}")
add_dependencies(scada-tag-retiring-queue-integration-test
  scada-tagd scada-tag-test-crosser)

# A killed test binary leaves no process and no directory behind (issue
# #64): the helper scada-test-killed-runner is a test binary that starts
# scada-tagd and two simulator processes and waits; the test kills it with
# SIGKILL and looks for what is left. The helper is not a CTest test.
add_executable(scada-test-killed-runner killed_runner.cpp)
target_link_libraries(scada-test-killed-runner PRIVATE
  scada::tag scada::tagd scada::sim scada::service scada::test-support
  GTest::gtest_main)
scada_target_defaults(scada-test-killed-runner)
scada_test_command(killed_runner_command scada-test-killed-runner)
scada_add_test(scada-test-cleanup-integration-test KIND integration
  SOURCES cleanup_integration_test.cpp
  DEPENDS scada::tagd scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_SIM_COMMAND=${sim_command}"
    "SCADA_KILLED_RUNNER_COMMAND=${killed_runner_command}")
add_dependencies(scada-test-cleanup-integration-test
  scada-tagd scada-sim scada-test-killed-runner)

# The driver process through real tags (decision D-142): scada-tagd, the
# test protocol's simulator and the test protocol's driver process.
scada_test_command(tagd_command scada-tagd)
scada_test_command(drv_test_command scada-drv-test)
scada_test_command(driver_sim_command scada-driver-test-sim)
scada_add_test(scada-driver-process-integration-test KIND integration
  SOURCES driver_integration_test.cpp driver_stop_integration_test.cpp
    driver_identity_integration_test.cpp
    driver_records_integration_test.cpp
    tag_writer_integration_test.cpp
  DEPENDS scada::tagd scada::tag scada::service scada::test-support
    scada::driver
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_DRV_TEST_COMMAND=${drv_test_command}"
    "SCADA_DRIVER_TEST_SIM_COMMAND=${driver_sim_command}")
add_dependencies(scada-driver-process-integration-test
  scada-tagd scada-drv-test scada-driver-test-sim)
# The TagWriter of a worker, tested in this process (its header is the
# library's own, not installed).
target_include_directories(scada-driver-process-integration-test PRIVATE
  ${PROJECT_SOURCE_DIR}/libs/scada-driver/src)
