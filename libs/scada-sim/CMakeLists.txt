scada_add_library(scada-sim
  SOURCES
    src/layout.cpp
    src/pattern.cpp
    src/sim.cpp
    src/tag_set.cpp
    src/writer.cpp
  DEPENDS scada::tag)

if(BUILD_TESTING)
  add_subdirectory(tests)
  add_subdirectory(fuzz)
endif()
