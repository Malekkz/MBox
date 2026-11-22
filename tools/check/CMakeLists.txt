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

# Empty: clang-tidy lints every translation unit. A revision: it lints only
# the units the changes since its merge base with HEAD could have affected,
# and everything if that cannot be worked out. The pipeline sets it on a pull
# request (docs/decisions.md, D-67). The other checks run on every file
# either way.
set(SCADA_CHECK_TIDY_BASE "" CACHE STRING
  "check: lint only what changed since this revision (empty: everything)")
set(scada_check_tidy_base "")
if(SCADA_CHECK_TIDY_BASE)
  set(scada_check_tidy_base --tidy-base "${SCADA_CHECK_TIDY_BASE}")
endif()

# For the pipeline, which runs the checks in several jobs (D-68). Empty, the
# default: every check, and clang-tidy lints all of its units in one run.
# SCADA_CHECK_ONLY runs only the listed checks (format, tidy, banned, rt).
# SCADA_CHECK_TIDY_SHARE "K/N" lints only share K of N of the units, needs
# SCADA_CHECK_ONLY "tidy", and writes what it linted to
# SCADA_CHECK_TIDY_RECORD if set.
set(SCADA_CHECK_ONLY "" CACHE STRING
  "check: run only these checks, a list of format, tidy, banned and rt (empty: all)")
set(SCADA_CHECK_TIDY_SHARE "" CACHE STRING
  "check: lint only share K/N of the units clang-tidy lints (empty: all)")
set(SCADA_CHECK_TIDY_RECORD "" CACHE FILEPATH
  "check: with SCADA_CHECK_TIDY_SHARE, write what the share linted here")
set(scada_check_split "")
foreach(check IN LISTS SCADA_CHECK_ONLY)
  list(APPEND scada_check_split --only "${check}")
endforeach()
if(SCADA_CHECK_TIDY_SHARE)
  list(APPEND scada_check_split --tidy-share "${SCADA_CHECK_TIDY_SHARE}")
endif()
if(SCADA_CHECK_TIDY_RECORD)
  list(APPEND scada_check_split --tidy-record "${SCADA_CHECK_TIDY_RECORD}")
endif()

set(scada_check_tools
  --clang-format "${SCADA_CLANG_FORMAT}"
  --clang-tidy "${SCADA_CLANG_TIDY}"
  --clang-query "${SCADA_CLANG_QUERY}")

# The fixtures break the rules on purpose; the tests below cover them. The
# sanitizer fixtures are deliberate bugs too, and exist only in the sanitizer
# builds (tests/sanitizer-fixtures/CMakeLists.txt). The model fixtures are
# compiler input kept for their disassembly, not part of any build
# (tests/model/README.md).
set(scada_check_excludes
  --exclude tools/check/tests/fixtures
  --exclude tests/sanitizer-fixtures
  --exclude tests/model/fixtures)
add_custom_target(check
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scada_check.py"
    --source-dir "${PROJECT_SOURCE_DIR}"
    --build-dir "${PROJECT_BINARY_DIR}"
    ${scada_check_excludes}
    ${scada_check_tidy_base}
    ${scada_check_split}
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
  # About 15 s, most of it in clang-tidy; longer on a loaded machine.
  set_tests_properties(scada-check-test PROPERTIES LABELS unit TIMEOUT 300)
  # The selection of the units clang-tidy lints on a pull request. It
  # compiles small files with the C++ compiler of this build to get their
  # dependencies.
  add_test(NAME scada-check-selection-test
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/selection_test.py"
      --compiler "${CMAKE_CXX_COMPILER}" ${scada_check_tools})
  set_tests_properties(scada-check-selection-test PROPERTIES
    LABELS unit TIMEOUT 120)
  # The split of the lint across the jobs of the pipeline (D-68), on its
  # own lists and on the translation units of this build, found as the
  # check target finds them. No tools; under a second.
  add_test(NAME scada-check-shares-test
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/shares_test.py"
      --source-dir "${PROJECT_SOURCE_DIR}"
      --build-dir "${PROJECT_BINARY_DIR}"
      ${scada_check_excludes})
  set_tests_properties(scada-check-shares-test PROPERTIES
    LABELS unit TIMEOUT 60)
endif()
