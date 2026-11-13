# An empty list gives an empty, well-formed bill of materials and report.
include("${CMAKE_CURRENT_LIST_DIR}/check_helpers.cmake")

file(READ "${REPORT_DIR}/sbom.cdx.json" sbom)
expect_json("${sbom}" "CycloneDX" bomFormat)
expect_json("${sbom}" "1.5" specVersion)
expect_json("${sbom}" "fixture" metadata component name)
expect_json("${sbom}" "1.2.3" metadata component version)
string(JSON count LENGTH "${sbom}" components)
if(NOT count EQUAL 0)
  message(FATAL_ERROR "Expected no components, got ${count}.")
endif()

file(READ "${REPORT_DIR}/licence-report.md" report)
expect_contains("${report}" "No third-party dependencies.")
