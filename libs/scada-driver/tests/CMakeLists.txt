scada_add_test(scada-driver-unit-test KIND unit
  SOURCES
    failure_test.cpp
    golden_samples_test.cpp
    golden_vectors_test.cpp
    golden_write_test.cpp
    loopback.cpp
    plan_property_test.cpp
    plan_test.cpp
    point_rules_test.cpp
    sample_processing_test.cpp
    channel_engine_test.cpp
    connections_test.cpp
    diagnostics_test.cpp
    driver_test.cpp
    drv_test_support.cpp
    drv_test_support_test.cpp
    engine_random_test.cpp
    identity_test.cpp
    identity_text_test.cpp
    scheduling_test.cpp
    sim_transport.cpp
    stall_watch_test.cpp
    text_pattern_test.cpp
    test_protocol.cpp
    timing_test.cpp
    tzif_test.cpp
    zone_file_test.cpp
    zone_table_test.cpp
  DEPENDS scada::driver)
# The tests compute expected values with the same arithmetic as the library
# (a read of the raw value written), so they too are compiled without
# floating-point contraction (decision D-98); otherwise ARM64 fuses the
# test's own multiply and add and the expected value differs in the last
# bit.
target_compile_options(scada-driver-unit-test PRIVATE -ffp-contract=off)
target_compile_definitions(scada-driver-unit-test PRIVATE
  "SCADA_DRIVER_GOLDEN_VECTORS=\"${CMAKE_CURRENT_SOURCE_DIR}/golden/driver-vectors.txt\"")

# How dead devices share a line with live ones, with the default failure
# keys (decision D-138; review G7, N1): minutes of a simulated line.
scada_add_test(scada-driver-line-perf-test KIND perf
  SOURCES
    line_perf_test.cpp
    loopback.cpp
    sim_transport.cpp
    test_protocol.cpp
  DEPENDS scada::driver)

# Every zone of the build machine's database against the C library
# (decision D-130). Integration: it reads hundreds of files and takes
# seconds.
scada_add_test(scada-driver-zones-test KIND integration
  SOURCES zone_table_all_zones_test.cpp
  DEPENDS scada::driver)
target_compile_definitions(scada-driver-zones-test PRIVATE
  "SCADA_ZONEINFO_DIR=\"${SCADA_ZONEINFO_DIR}\"")

# The test protocol's simulator as a process of its own (contract 14), for
# the integration test of the real transports.
add_executable(scada-driver-test-sim
  test_sim_main.cpp loopback.cpp test_protocol.cpp)
# loopback.cpp checks its inputs with GoogleTest assertions.
target_link_libraries(scada-driver-test-sim PRIVATE scada::driver GTest::gtest)
scada_target_defaults(scada-driver-test-sim)

# The TCP and serial transports across the boundary: real sockets against
# the simulator process and against the test, a pseudo-terminal pair for
# the serial port (decision D-141).
set(sim_command ${CMAKE_CROSSCOMPILING_EMULATOR}
  "$<TARGET_FILE:scada-driver-test-sim>")
list(JOIN sim_command "|" sim_command)
scada_add_test(scada-driver-transport-test KIND integration
  SOURCES
    transport_integration_test.cpp
    loopback.cpp
    sim_transport.cpp
    test_protocol.cpp
  DEPENDS scada::driver scada::test-support
  ENVIRONMENT "SCADA_DRIVER_TEST_SIM_COMMAND=${sim_command}")
add_dependencies(scada-driver-transport-test scada-driver-test-sim)

# A driver process of the test protocol, for the integration tests of the
# driver process in tests/integration (decision D-142). Not installed.
# Its logic is a library of its own, so that the unit tests reach it.
add_library(scada-drv-test-support STATIC drv_test_support.cpp test_protocol.cpp)
target_link_libraries(scada-drv-test-support PUBLIC scada::driver)
target_include_directories(scada-drv-test-support PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR})
scada_target_defaults(scada-drv-test-support)
add_executable(scada-drv-test drv_test_main.cpp)
target_link_libraries(scada-drv-test PRIVATE scada-drv-test-support)
scada_target_defaults(scada-drv-test)
