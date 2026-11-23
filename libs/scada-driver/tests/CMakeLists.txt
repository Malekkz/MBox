scada_add_test(scada-driver-unit-test KIND unit
  SOURCES
    golden_samples_test.cpp
    golden_write_test.cpp
    sample_processing_test.cpp
    zone_table_test.cpp
  DEPENDS scada::driver)
