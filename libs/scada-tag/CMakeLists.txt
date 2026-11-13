scada_add_library(scada-tag
  SOURCES
    src/layout.cpp
  DEPENDS scada::common)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
