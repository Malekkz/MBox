# A project with no compiled code, used to test ScadaDependencies.cmake.
# FIXTURE_LINKS is what the project's own target links, and
# FIXTURE_USES_VERSION_OF a dependency whose listed version the build reads.
cmake_minimum_required(VERSION 3.21)
project(fixture VERSION 1.2.3 LANGUAGES NONE)

list(APPEND CMAKE_MODULE_PATH "${SCADA_CMAKE_DIR}")
include(ScadaDependencies)
scada_dependencies(
  LIST "${FIXTURE_LIST}"
  ALLOWLIST "${FIXTURE_ALLOWLIST}"
  OUTPUT_DIR "${CMAKE_BINARY_DIR}/reports")

# Stands for build code that fetches a dependency at the listed version.
if(FIXTURE_USES_VERSION_OF)
  scada_dependency_version(fetched_version "${FIXTURE_USES_VERSION_OF}")
  if(NOT fetched_version STREQUAL "1.4.0")
    message(FATAL_ERROR "Got version '${fetched_version}', expected 1.4.0")
  endif()
endif()

# Stand-ins for packages found on the system.
add_library(external::alpha INTERFACE IMPORTED)
add_library(external::beta INTERFACE IMPORTED)

add_library(own-base INTERFACE)
add_library(fixture::base ALIAS own-base)
add_library(own INTERFACE)
target_link_libraries(own INTERFACE fixture::base ${FIXTURE_LINKS})
