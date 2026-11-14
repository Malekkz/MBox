scada_add_library(scada-tag
  SOURCES
    src/generation_host.cpp
    src/header_builder.cpp
    src/layout.cpp
    src/proc_region.cpp
    src/region_file.cpp
    src/sha256.cpp
    src/tag_set.cpp
    src/tag_set_json.cpp
  DEPENDS scada::common)

if(BUILD_TESTING)
  add_subdirectory(tests)
  add_subdirectory(fuzz)
endif()
