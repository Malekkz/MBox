find_package(Threads REQUIRED)

scada_add_library(scada-common
  SOURCES
    src/clock.cpp
    src/fd.cpp
    src/log_sinks.cpp
    src/log_text.cpp
    src/logger.cpp
    src/mapped_region.cpp
    src/panic.cpp
    src/status.cpp
  DEPENDS Threads::Threads)

# The pause instruction of `scada::cpu_relax()` on ARM64 (decision D-31):
# `isb`, or `yield` to measure a board both ways. A definition of the whole
# build, public so that every user of the header sees the same choice.
# x86-64 ignores it.
set(SCADA_ARM64_PAUSE "isb" CACHE STRING
  "Instruction of scada::cpu_relax() on ARM64: isb or yield")
set_property(CACHE SCADA_ARM64_PAUSE PROPERTY STRINGS isb yield)
if(SCADA_ARM64_PAUSE STREQUAL "yield")
  target_compile_definitions(libscada-common PUBLIC SCADA_ARM64_PAUSE_YIELD)
elseif(NOT SCADA_ARM64_PAUSE STREQUAL "isb")
  message(FATAL_ERROR
    "SCADA_ARM64_PAUSE must be isb or yield, got '${SCADA_ARM64_PAUSE}'")
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
  add_subdirectory(fuzz)
endif()
