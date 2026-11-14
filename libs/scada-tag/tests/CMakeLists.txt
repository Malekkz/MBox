scada_add_test(scada-tag-unit-test KIND unit
  SOURCES
    attachment_test.cpp
    c_api_test.cpp
    change_ring_test.cpp
    generation_host_test.cpp
    handle_test.cpp
    header_builder_test.cpp
    header_view_test.cpp
    layout_test.cpp
    name_hash_test.cpp
    own_region_test.cpp
    peer_regions_test.cpp
    proc_region_test.cpp
    region_file_test.cpp
    sha256_test.cpp
    subscription_test.cpp
    tag_access_test.cpp
    tag_record_test.cpp
    tag_set_json_test.cpp
    tag_set_test.cpp
    types_test.cpp
  DEPENDS scada::tag)
# The headers in src/ are private to the library.
target_include_directories(scada-tag-unit-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# Stands in for a service in the integration tests: it attaches through the
# C interface. It uses the private headers, as the unit tests do, to leave a
# tag record as a killed writer does, and is not installed.
add_executable(scada-tag-test-peer peer/peer_main.cpp)
target_link_libraries(scada-tag-test-peer PRIVATE scada::tag)
target_include_directories(scada-tag-test-peer
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
scada_target_defaults(scada-tag-test-peer)

# The writing side of conformance tests 2 and 3: one process with several
# owner threads that write derived values at a given rate. Not installed.
add_executable(scada-tag-test-writer writer/writer_main.cpp)
target_link_libraries(scada-tag-test-writer PRIVATE scada::tag)
scada_target_defaults(scada-tag-test-writer)

# The program conformance test 22 traces: it calls the real-time safe
# functions between two markers and counts its own allocations. Not
# installed.
add_executable(scada-tag-rt-probe probe/rt_probe_main.cpp)
target_link_libraries(scada-tag-rt-probe
  PRIVATE scada::tag scada::test-support)
scada_target_defaults(scada-tag-rt-probe)
