scada_add_library(scada-tag
  SOURCES
    src/header_builder.cpp
    src/layout.cpp
    src/sha256.cpp
    src/tag_set.cpp
    src/tag_set_json.cpp
  DEPENDS scada::common)

if(BUILD_TESTING)
  add_subdirectory(tests)
  add_subdirectory(fuzz)
endif()
