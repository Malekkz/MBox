scada_add_executable(scada-hello SOURCES src/main.cpp DEPENDS scada::hello)

if(BUILD_TESTING)
  # Runs the real executable, so it also covers linking and start-up.
  add_test(NAME scada-hello-runs COMMAND scada-hello)
  set_tests_properties(scada-hello-runs PROPERTIES
    PASS_REGULAR_EXPRESSION "^Hello from libscada-hello\n$")

  add_test(NAME scada-hello-rejects-unknown-option COMMAND scada-hello --bogus)
  set_tests_properties(scada-hello-rejects-unknown-option PROPERTIES
    WILL_FAIL TRUE)
endif()
