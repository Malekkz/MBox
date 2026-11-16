scada_add_test(scada-tagd-unit-test KIND unit
  SOURCES late_log_test.cpp report_log_test.cpp suspend_test.cpp tagd_test.cpp
    wait_test.cpp
  DEPENDS scada::tagd)
# The headers in src/ are private to the library.
target_include_directories(scada-tagd-unit-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# scada-tagd with a retain writer that hangs in fsync, for the integration
# test of the bounded stop (decision D-59). Test code; not installed.
add_executable(scada-tagd-test-hung-writer hung_writer/hung_writer_main.cpp)
target_link_libraries(scada-tagd-test-hung-writer PRIVATE scada::tagd scada::tag)
target_include_directories(scada-tagd-test-hung-writer
  PRIVATE "${PROJECT_SOURCE_DIR}/libs/scada-tag/src")
scada_target_defaults(scada-tagd-test-hung-writer)
