# Configures the fixture project for one case and checks the outcome.
#
#   cmake -DSOURCE_DIR=... -DBINARY_DIR=... -DGENERATOR=... -DMODULE_DIR=...
#         -DLIST=... -DALLOWLIST=... [-DLINKS=a|b] [-DUSES_VERSION_OF=<name>]
#         [-DEXPECT_ERROR=<regex>]
#         [-DCHECK_SCRIPT=<file>] -P run_case.cmake
#
# With EXPECT_ERROR the configuration must fail with matching output.
# Without it the configuration must succeed, and CHECK_SCRIPT, if given, is
# included to inspect the files in REPORT_DIR.

# LINKS separates names with "|" and writes "$<" as "@<" (see CMakeLists.txt).
string(REPLACE "|" ";" links "${LINKS}")
string(REPLACE "@<" "$<" links "${links}")
string(REPLACE ";" "\;" links "${links}")

file(REMOVE_RECURSE "${BINARY_DIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${BINARY_DIR}"
    -G "${GENERATOR}"
    "-DSCADA_CMAKE_DIR=${MODULE_DIR}"
    "-DFIXTURE_LIST=${LIST}"
    "-DFIXTURE_ALLOWLIST=${ALLOWLIST}"
    "-DFIXTURE_LINKS=${links}"
    "-DFIXTURE_USES_VERSION_OF=${USES_VERSION_OF}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output)

if(DEFINED EXPECT_ERROR)
  if(result EQUAL 0)
    message(FATAL_ERROR "Configuration succeeded but had to fail.\n${output}")
  endif()
  if(NOT output MATCHES "${EXPECT_ERROR}")
    message(FATAL_ERROR
      "Configuration failed without the expected message "
      "'${EXPECT_ERROR}'.\n${output}")
  endif()
  # A failed check must not leave a report behind that looks valid.
  file(GLOB left_behind "${BINARY_DIR}/reports/*")
  if(left_behind)
    message(FATAL_ERROR "The failed check left files behind: ${left_behind}")
  endif()
  return()
endif()

if(NOT result EQUAL 0)
  message(FATAL_ERROR "Configuration failed.\n${output}")
endif()
set(REPORT_DIR "${BINARY_DIR}/reports")
foreach(file IN ITEMS licence-report.md sbom.cdx.json)
  if(NOT EXISTS "${REPORT_DIR}/${file}")
    message(FATAL_ERROR "${REPORT_DIR}/${file} was not written.")
  endif()
endforeach()
if(DEFINED CHECK_SCRIPT)
  include("${CHECK_SCRIPT}")
endif()
