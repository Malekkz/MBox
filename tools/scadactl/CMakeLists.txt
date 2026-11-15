# scadactl: the local command-line tool (reference section 11.5). All logic
# is in libscadactl; the executable only hands it the arguments.
scada_add_library(scadactl
  SOURCES src/format.cpp src/pattern.cpp
  DEPENDS scada::tag)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
