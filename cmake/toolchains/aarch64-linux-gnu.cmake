# Cross-compilation from any host to 64-bit ARM Linux with a GNU toolchain.
#
# The compiler is found on PATH by its target-triplet prefix, which is the
# upstream GNU naming and not tied to a distribution. Set SCADA_CROSS_PREFIX
# (cache variable or environment) for a toolchain with a different prefix, for
# example an absolute path ending in `aarch64-none-linux-gnu-`.
#
# To run the tests of a cross build, pass an emulator at configure time:
#   -DCMAKE_CROSSCOMPILING_EMULATOR="<emulator>;<arguments>"

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED SCADA_CROSS_PREFIX)
  if(DEFINED ENV{SCADA_CROSS_PREFIX})
    set(SCADA_CROSS_PREFIX "$ENV{SCADA_CROSS_PREFIX}")
  else()
    set(SCADA_CROSS_PREFIX "aarch64-linux-gnu-")
  endif()
endif()
# CMake re-reads this file in try-compile projects, where cache variables of
# the main project are not visible; forward the prefix.
set(ENV{SCADA_CROSS_PREFIX} "${SCADA_CROSS_PREFIX}")

set(CMAKE_CXX_COMPILER "${SCADA_CROSS_PREFIX}g++")

# Look for programs on the host, and for libraries and headers only in the
# target environment.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
