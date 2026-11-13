find_package(Threads REQUIRED)

scada_add_library(scada-common
  SOURCES
    src/clock.cpp
    src/fd.cpp
    src/log_sinks.cpp
    src/log_text.cpp
    src/logger.cpp
    src/panic.cpp
    src/status.cpp
  DEPENDS Threads::Threads)

if(BUILD_TESTING)
  add_subdirectory(tests)
  add_subdirectory(fuzz)
endif()
