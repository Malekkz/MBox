scada_add_test(scada-tagd-unit-test KIND unit
  SOURCES late_log_test.cpp report_log_test.cpp tagd_test.cpp
  DEPENDS scada::tagd)
# The headers in src/ are private to the library.
target_include_directories(scada-tagd-unit-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
