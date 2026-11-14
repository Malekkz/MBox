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
