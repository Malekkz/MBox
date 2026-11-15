# scadactl: the local command-line tool (reference section 11.5). All logic
# is in libscadactl; the executable only hands it the arguments.
scada_add_library(scadactl
  SOURCES
    src/arguments.cpp
    src/database.cpp
    src/format.cpp
    src/pattern.cpp
    src/scadactl.cpp
    src/view.cpp
    src/watch.cpp
  DEPENDS scada::tag)

scada_add_executable(scadactl SOURCES src/main.cpp DEPENDS scada::scadactl)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
