scada_add_test(scada-driver-unit-test KIND unit
  SOURCES
    golden_samples_test.cpp
    golden_vectors_test.cpp
    golden_write_test.cpp
    point_rules_test.cpp
    sample_processing_test.cpp
    tzif_test.cpp
    zone_table_test.cpp
  DEPENDS scada::driver)
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
