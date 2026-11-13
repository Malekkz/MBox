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
include(ScadaTargets)

add_subdirectory(libs)
add_subdirectory(services)
add_subdirectory(tools)
add_subdirectory(tests)
