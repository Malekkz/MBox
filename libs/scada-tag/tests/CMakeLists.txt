scada_add_test(scada-tag-unit-test KIND unit
  SOURCES
    generation_host_test.cpp
    handle_test.cpp
    header_builder_test.cpp
    layout_test.cpp
    name_hash_test.cpp
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

# Stands in for a service in the integration tests of scada-tagd, until the
# library can attach a process (issue P1-05). It uses the private headers,
# as the unit tests do, and is not installed.
add_executable(scada-tag-test-peer peer/peer_main.cpp)
target_link_libraries(scada-tag-test-peer PRIVATE scada::tag)
target_include_directories(scada-tag-test-peer
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
scada_target_defaults(scada-tag-test-peer)
