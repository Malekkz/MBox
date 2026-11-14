scada_add_test(scada-tag-unit-test KIND unit
  SOURCES
    attachment_test.cpp
    c_api_test.cpp
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
