scada_add_test(scada-common-unit-test KIND unit
  SOURCES
    clock_test.cpp
    fd_test.cpp
    log_sink_test.cpp
    log_queue_test.cpp
    log_ring_test.cpp
    log_test.cpp
    mapped_region_test.cpp
    realtime_test.cpp
    result_test.cpp
    status_test.cpp
  DEPENDS scada::common)
# log_internal.h: helpers that are not part of the public interface.
target_include_directories(scada-common-unit-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
