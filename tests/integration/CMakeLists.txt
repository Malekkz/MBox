# Integration tests: each starts real processes.

# The command that starts scada-hello, elements separated by `|`. A cross
# build runs its programs through the emulator, and so must the test.
set(hello_command ${CMAKE_CROSSCOMPILING_EMULATOR} "$<TARGET_FILE:scada-hello>")
list(JOIN hello_command "|" hello_command)

scada_add_test(scada-hello-integration-test KIND integration
  SOURCES hello_integration_test.cpp
  DEPENDS scada::hello scada::test-support
  ENVIRONMENT "SCADA_HELLO_COMMAND=${hello_command}")
