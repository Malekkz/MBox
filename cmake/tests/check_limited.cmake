# A licence limited to some scopes is accepted in one of them, and the
# licence report says which scopes it is limited to.
include("${CMAKE_CURRENT_LIST_DIR}/check_helpers.cmake")

file(READ "${REPORT_DIR}/licence-report.md" report)
expect_contains("${report}" "| alpha | 1.4.0 | CECILL-B | test |")
expect_contains("${report}" "CECILL-B (scope build and test only)")
