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
    scheduling_test.cpp
    sim_transport.cpp
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

# Every zone of the build machine's database against the C library
# (decision D-130). Integration: it reads hundreds of files and takes
# seconds.
scada_add_test(scada-driver-zones-test KIND integration
  SOURCES zone_table_all_zones_test.cpp
  DEPENDS scada::driver)
target_compile_definitions(scada-driver-zones-test PRIVATE
  "SCADA_ZONEINFO_DIR=\"${SCADA_ZONEINFO_DIR}\"")
