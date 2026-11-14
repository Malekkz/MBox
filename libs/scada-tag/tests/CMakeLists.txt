scada_add_test(scada-tag-unit-test KIND unit
  SOURCES
    handle_test.cpp
    header_builder_test.cpp
    layout_test.cpp
    name_hash_test.cpp
    sha256_test.cpp
    tag_set_json_test.cpp
    tag_set_test.cpp
    types_test.cpp
  DEPENDS scada::tag)
# layout.h, name_hash.h and sha256.h are private to the library.
target_include_directories(scada-tag-unit-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
