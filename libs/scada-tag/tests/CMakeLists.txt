scada_add_test(scada-tag-unit-test KIND unit
  SOURCES
    handle_test.cpp
    layout_test.cpp
    name_hash_test.cpp
    types_test.cpp
  DEPENDS scada::tag)
# layout.h and name_hash.h are private to the library.
target_include_directories(scada-tag-unit-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
