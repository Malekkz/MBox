cmake_minimum_required(VERSION 3.21)

project(scada VERSION 0.1.0 DESCRIPTION "Embedded SCADA node" LANGUAGES CXX)

# C++20 with no compiler extensions (reference 4.1).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CTest)
include(ScadaSanitizers)
include(ScadaTargets)
include(ScadaDependencies)

# Third-party dependency list, licence check, licence report and bill of
# materials (reference 13.6, 16.3). Must precede the first target.
set(SCADA_REPORT_DIR "${CMAKE_BINARY_DIR}/reports")
scada_dependencies(
  LIST "${CMAKE_CURRENT_SOURCE_DIR}/third-party/dependencies.json"
  ALLOWLIST "${CMAKE_CURRENT_SOURCE_DIR}/third-party/licence-allowlist.json"
  OUTPUT_DIR "${SCADA_REPORT_DIR}")
install(FILES
  "${SCADA_REPORT_DIR}/licence-report.md"
  "${SCADA_REPORT_DIR}/sbom.cdx.json"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}")

if(BUILD_TESTING)
  include(ScadaTesting)
endif()

add_subdirectory(libs)
add_subdirectory(services)
add_subdirectory(tools)
add_subdirectory(tests)
if(BUILD_TESTING)
  add_subdirectory(cmake/tests)
endif()
