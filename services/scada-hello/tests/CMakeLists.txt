# Starts the real executable, with the test standing in for systemd. For a
# cross build the emulator, if one is configured, runs the service.
scada_add_test(scada-hello-service-test
  SOURCES service_test.cpp
  DEPENDS scada::hello scada::service
  ARGS ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:scada-hello>)
set_tests_properties(scada-hello-service-test PROPERTIES TIMEOUT 60)
