# Headers only for now, so this is an interface library. Issue P0-06 adds the
# first sources and turns it into a normal library with scada_add_library.
add_library(libscada-common INTERFACE)
target_include_directories(libscada-common INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
add_library(scada::common ALIAS libscada-common)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
