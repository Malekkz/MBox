# Helpers that create targets following the library-plus-thin-executable
# pattern (CLAUDE.md, How work is organised).

include(ScadaCompileOptions)

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
# Creates an executable and installs it to the binary directory of the install
# tree.
function(scada_add_executable name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEPENDS")
  if(arg_UNPARSED_ARGUMENTS OR NOT arg_SOURCES)
    message(FATAL_ERROR "scada_add_executable(${name}): bad arguments")
  endif()

  add_executable(${name} ${arg_SOURCES})
  target_link_libraries(${name} PRIVATE ${arg_DEPENDS})
  scada_target_defaults(${name})
  install(TARGETS ${name} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endfunction()

# scada_add_test(<name> KIND <unit|integration|perf> SOURCES <files...>
#                [DEPENDS <targets...>] [ENVIRONMENT <NAME=value...>])
#
# Creates a GoogleTest executable and registers it with CTest as one test,
# labelled with its kind. It is not installed. The framework supplies main().
#
# The executable is one CTest test, not one per test case, so that the test
# list does not have to be read out of the binary at build time; that would
# not work for a cross build without an emulator.
function(scada_add_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "KIND" "SOURCES;DEPENDS;ENVIRONMENT")
  if(arg_UNPARSED_ARGUMENTS OR NOT arg_SOURCES)
    message(FATAL_ERROR "scada_add_test(${name}): bad arguments")
  endif()

  # Seconds. A test that exceeds its limit is a failure, not a slow test.
  if(arg_KIND STREQUAL "unit")
    set(timeout 60)
  elseif(arg_KIND STREQUAL "integration")
    set(timeout 300)
  elseif(arg_KIND STREQUAL "perf")
    set(timeout 600)
  else()
    message(FATAL_ERROR
      "scada_add_test(${name}): KIND must be unit, integration or perf")
  endif()

  add_executable(${name} ${arg_SOURCES})
  target_link_libraries(${name} PRIVATE ${arg_DEPENDS} GTest::gtest_main)
  scada_target_defaults(${name})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    LABELS ${arg_KIND}
    TIMEOUT ${timeout}
    ENVIRONMENT "${SCADA_TEST_ENVIRONMENT};${arg_ENVIRONMENT}")
endfunction()

# scada_add_fuzz_target(<name> SOURCES <files...> [DEPENDS <targets...>]
#                       [EXPECT_FINDING <regex>])
#
# Creates a fuzz target: an executable whose sources define
# LLVMFuzzerTestOneInput. The seed corpus is the directory `corpus/<name>/`
# next to the calling CMakeLists.txt and must exist.
#
# With SCADA_SANITIZER=fuzz the target is linked with libFuzzer and the test
# `<name>` (label `fuzz`) runs it for SCADA_FUZZ_SECONDS; any crash, sanitizer
# report or input that takes longer than 10 s fails it. In every other build
# the target is linked with a driver that replays the seed corpus once, as the
# unit test `<name>-corpus`, so that the corpus is also a regression test on
# both architectures and under the other sanitizers.
#
# EXPECT_FINDING inverts the fuzz run: the test passes only if the fuzzer
# stops with a report matching <regex> within the time. It exists for the
# sample that proves fuzzing works, not for product code.
function(scada_add_fuzz_target name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "EXPECT_FINDING" "SOURCES;DEPENDS")
  if(arg_UNPARSED_ARGUMENTS OR NOT arg_SOURCES)
    message(FATAL_ERROR "scada_add_fuzz_target(${name}): bad arguments")
  endif()
  set(seed_corpus "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name}")
  if(NOT IS_DIRECTORY "${seed_corpus}")
    message(FATAL_ERROR
      "scada_add_fuzz_target(${name}): no seed corpus at ${seed_corpus}")
  endif()

  add_executable(${name} ${arg_SOURCES})
  target_link_libraries(${name} PRIVATE ${arg_DEPENDS})
  scada_target_defaults(${name})

  if(NOT SCADA_SANITIZER STREQUAL "fuzz")
    target_link_libraries(${name} PRIVATE scada::fuzz-replay)
    add_test(NAME ${name}-corpus COMMAND ${name} "${seed_corpus}")
    set_tests_properties(${name}-corpus PROPERTIES
      LABELS unit
      TIMEOUT 60
      ENVIRONMENT "${SCADA_TEST_ENVIRONMENT}")
    return()
  endif()

  target_link_options(${name} PRIVATE -fsanitize=fuzzer)

  # Inputs the fuzzer discovers and the inputs that made it fail go to the
  # build tree; the seed corpus in the source tree is only read.
  set(work_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}-work")
  set(found_corpus "${work_dir}/corpus")
  set(artifacts "${work_dir}/artifacts")
  file(MAKE_DIRECTORY "${found_corpus}" "${artifacts}")
  set(fuzz_command
    "$<TARGET_FILE:${name}>"
    "-max_total_time=${SCADA_FUZZ_SECONDS}"
    "-timeout=10"
    "-artifact_prefix=${artifacts}/"
    "${found_corpus}"
    "${seed_corpus}")

  if(arg_EXPECT_FINDING)
    # Start from the seed corpus alone each time, so that the result does not
    # depend on what an earlier run left behind.
    list(JOIN fuzz_command "|" joined_command)
    add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}"
      "-DSCADA_COMMAND=${joined_command}"
      "-DSCADA_REGEX=${arg_EXPECT_FINDING}"
      "-DSCADA_FRESH_DIRS=${found_corpus}|${artifacts}"
      -P "${PROJECT_SOURCE_DIR}/cmake/ExpectFinding.cmake")
  else()
    add_test(NAME ${name} COMMAND ${fuzz_command})
  endif()
  math(EXPR timeout "${SCADA_FUZZ_SECONDS} + 60")
  set_tests_properties(${name} PROPERTIES
    LABELS fuzz
    TIMEOUT ${timeout}
    ENVIRONMENT "${SCADA_TEST_ENVIRONMENT}")
endfunction()
