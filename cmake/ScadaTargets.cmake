# Helpers that create targets following the library-plus-thin-executable
# pattern (CLAUDE.md, How work is organised).

include(ScadaCompileOptions)
include(ScadaInstall)

# scada_add_library(<name> SOURCES <files...> [DEPENDS <targets...>])
#
# Creates the static library lib<name>.a as target `lib<name>` with the alias
# `scada::<name without the scada- prefix>`. Public headers are expected in
# `include/` next to the calling CMakeLists.txt.
#
# The target is called `lib<name>` because a service executable usually has
# the same name as its library, and target names must be unique.
function(scada_add_library name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEPENDS")
  if(arg_UNPARSED_ARGUMENTS OR NOT arg_SOURCES)
    message(FATAL_ERROR "scada_add_library(${name}): bad arguments")
  endif()

  set(target "lib${name}")
  add_library(${target} STATIC ${arg_SOURCES})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME "${name}")
  target_include_directories(${target} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
  target_link_libraries(${target} PUBLIC ${arg_DEPENDS})
  scada_target_defaults(${target})

  string(REGEX REPLACE "^scada-" "" alias "${name}")
  add_library("scada::${alias}" ALIAS ${target})
endfunction()

# scada_add_executable(<name> SOURCES <files...> [DEPENDS <targets...>])
#
# Creates an executable and installs it to the binary directory of the staged
# tree (ScadaInstall.cmake).
function(scada_add_executable name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEPENDS")
  if(arg_UNPARSED_ARGUMENTS OR NOT arg_SOURCES)
    message(FATAL_ERROR "scada_add_executable(${name}): bad arguments")
  endif()

  add_executable(${name} ${arg_SOURCES})
  target_link_libraries(${name} PRIVATE ${arg_DEPENDS})
  scada_target_defaults(${name})
  install(TARGETS ${name} RUNTIME DESTINATION "${SCADA_INSTALL_BINDIR}")
endfunction()

# scada_add_test(<name> SOURCES <files...> [DEPENDS <targets...>]
#                [ARGS <arguments...>])
#
# Creates a test executable and registers it with CTest, run with ARGS. It is
# not installed.
function(scada_add_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEPENDS;ARGS")
  if(arg_UNPARSED_ARGUMENTS OR NOT arg_SOURCES)
    message(FATAL_ERROR "scada_add_test(${name}): bad arguments")
  endif()

  add_executable(${name} ${arg_SOURCES})
  target_link_libraries(${name} PRIVATE ${arg_DEPENDS})
  scada_target_defaults(${name})
  add_test(NAME ${name} COMMAND ${name} ${arg_ARGS})
endfunction()
