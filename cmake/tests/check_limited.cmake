# A licence limited to some scopes is accepted in one of them, and the
# licence report says which scopes it is limited to.
include("${CMAKE_CURRENT_LIST_DIR}/check_helpers.cmake")

file(READ "${REPORT_DIR}/licence-report.md" report)
string(REGEX MATCH "\\| alpha \\| 1\\.4\\.0 \\| CECILL-B \\| (build|test) \\|" row "${report}")
if(NOT row)
  message(FATAL_ERROR "No row for alpha with CECILL-B and scope build or test.")
endif()
expect_contains("${report}" "CECILL-B (scope build and test only)")
