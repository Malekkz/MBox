# The `check` target: formatter, linter, banned-function and SCADA_RT checks
# (docs/coding-standard.md, section 13). See README.md in this directory.
#
# The tools are needed only for this target. A build without them works; the
# `check` target then fails and says what is missing.

find_package(Python3 3.8 COMPONENTS Interpreter)

# clang-format and clang-query are looked for next to clang-tidy first, so
# that all three come from the same LLVM installation.
find_program(SCADA_CLANG_TIDY clang-tidy DOC "Path of clang-tidy")
set(scada_llvm_hint "")
if(SCADA_CLANG_TIDY)
  file(REAL_PATH "${SCADA_CLANG_TIDY}" scada_clang_tidy_real)
  get_filename_component(scada_llvm_hint "${scada_clang_tidy_real}" DIRECTORY)
endif()
find_program(SCADA_CLANG_FORMAT clang-format HINTS ${scada_llvm_hint}
  DOC "Path of clang-format")
find_program(SCADA_CLANG_QUERY clang-query HINTS ${scada_llvm_hint}
  DOC "Path of clang-query")

set(scada_check_missing "")
if(NOT Python3_Interpreter_FOUND)
  list(APPEND scada_check_missing "Python 3.8 or newer")
endif()
foreach(tool IN ITEMS CLANG_FORMAT CLANG_TIDY CLANG_QUERY)
  if(NOT SCADA_${tool})
    string(TOLOWER "${tool}" tool_name)
    string(REPLACE "_" "-" tool_name "${tool_name}")
    list(APPEND scada_check_missing
      "${tool_name} (set SCADA_${tool} to its path)")
  endif()
endforeach()

if(scada_check_missing)
  list(JOIN scada_check_missing ", " scada_check_missing_text)
  message(WARNING
    "The 'check' target and its tests are not available. Not found: "
    "${scada_check_missing_text}.")
  add_custom_target(check
    COMMAND ${CMAKE_COMMAND} -E echo
      "check: cannot run. Not found: ${scada_check_missing_text}."
    COMMAND ${CMAKE_COMMAND} -E false
    VERBATIM)
  return()
endif()

set(scada_check_tools
  --clang-format "${SCADA_CLANG_FORMAT}"
  --clang-tidy "${SCADA_CLANG_TIDY}"
  --clang-query "${SCADA_CLANG_QUERY}")

# The fixtures break the rules on purpose; the tests below cover them.
add_custom_target(check
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scada_check.py"
    --source-dir "${PROJECT_SOURCE_DIR}"
    --build-dir "${PROJECT_BINARY_DIR}"
    --exclude tools/check/tests/fixtures
    ${scada_check_tools}
  COMMENT "Checking format, lint, banned functions and SCADA_RT"
  USES_TERMINAL
  VERBATIM)

if(BUILD_TESTING)
  # The interpreter is named by its path, not by its imported target: these
  # are tools of the build machine, and CTest would run an imported target
  # under the emulator of a cross build.
  add_test(NAME scada-check-test
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_test.py" ${scada_check_tools})
endif()
