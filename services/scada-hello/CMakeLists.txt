scada_add_executable(scada-hello SOURCES src/main.cpp DEPENDS scada::hello)
scada_install_service(scada-hello
  DESCRIPTION "MBox build and service skeleton check"
  WATCHDOG_SEC 10)

if(BUILD_TESTING)
  # These run the real executable, so they also cover linking and start-up.
  add_test(NAME scada-hello-prints-help COMMAND scada-hello --help)
  set_tests_properties(scada-hello-prints-help PROPERTIES
    PASS_REGULAR_EXPRESSION "^Usage: scada-hello")

  add_test(NAME scada-hello-rejects-unknown-option COMMAND scada-hello --bogus)
  set_tests_properties(scada-hello-rejects-unknown-option PROPERTIES
    WILL_FAIL TRUE)

  add_subdirectory(tests)
endif()
