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
# the tag database contract, the part of the reader.
scada_test_command(writer_command scada-tag-test-writer)
# The library checks the thread of an owner (contract 13.2) only in a build
# without NDEBUG; CMake defines NDEBUG in every build type but Debug.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(thread_check 1)
else()
  set(thread_check 0)
endif()
scada_add_test(scada-tag-access-integration-test KIND integration
  SOURCES tag_access_integration_test.cpp
  DEPENDS scada::tag scada::tagd scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_TAG_PEER_COMMAND=${peer_command}"
    "SCADA_TAG_WRITER_COMMAND=${writer_command}"
    "SCADA_TAG_THREAD_CHECK=${thread_check}")
add_dependencies(scada-tag-access-integration-test
  scada-tagd scada-tag-test-peer scada-tag-test-writer)

# The change rings with real processes: conformance tests 7 (the ring), 13
# and 24.
scada_add_test(scada-tag-change-ring-integration-test KIND integration
  SOURCES change_ring_integration_test.cpp
  DEPENDS scada::tag scada::tagd scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_TAG_PEER_COMMAND=${peer_command}"
    "SCADA_TAG_WRITER_COMMAND=${writer_command}")
add_dependencies(scada-tag-change-ring-integration-test
  scada-tagd scada-tag-test-peer scada-tag-test-writer)

# The reliable queues with real processes: conformance tests 16, 17 and
# 19, and one million records through one queue.
scada_test_command(queue_command scada-tag-test-queue)
scada_add_test(scada-tag-queue-integration-test KIND integration
  SOURCES queue_integration_test.cpp
  DEPENDS scada::tag scada::tagd scada::service scada::test-support
  ENVIRONMENT
    "SCADA_TAGD_COMMAND=${tagd_command}"
    "SCADA_TAG_QUEUE_COMMAND=${queue_command}"
    "SCADA_TAG_THREAD_CHECK=${thread_check}")
add_dependencies(scada-tag-queue-integration-test
  scada-tagd scada-tag-test-queue)

# Conformance test 22: the real-time safe functions make no system call and
# no allocation. The probe is traced as a child process. That shows nothing
# in a sanitizer build, whose runtime makes system calls of its own, and
# cannot work through an emulator, so the test exists only in a native
# build without a sanitizer.
if(NOT SCADA_SANITIZER AND NOT CMAKE_CROSSCOMPILING)
  scada_test_command(probe_command scada-tag-rt-probe)
  scada_add_test(scada-tag-rt-integration-test KIND integration
    SOURCES rt_safety_integration_test.cpp
    DEPENDS scada::tag scada::tagd scada::service scada::test-support
    ENVIRONMENT
      "SCADA_TAGD_COMMAND=${tagd_command}"
      "SCADA_TAG_PEER_COMMAND=${peer_command}"
      "SCADA_TAG_RT_PROBE_COMMAND=${probe_command}"
      "SCADA_TAG_QUEUE_COMMAND=${queue_command}")
  add_dependencies(scada-tag-rt-integration-test
    scada-tagd scada-tag-test-peer scada-tag-rt-probe scada-tag-test-queue)
endif()
