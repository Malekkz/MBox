# Both entries of cases/listed appear in the bill of materials and report.
include("${CMAKE_CURRENT_LIST_DIR}/check_helpers.cmake")

file(READ "${REPORT_DIR}/sbom.cdx.json" sbom)
string(JSON count LENGTH "${sbom}" components)
if(NOT count EQUAL 2)
  message(FATAL_ERROR "Expected 2 components, got ${count}.")
endif()
expect_json("${sbom}" "alpha" components 0 name)
expect_json("${sbom}" "1.4.0" components 0 version)
expect_json("${sbom}" "alpha@1.4.0" components 0 bom-ref)
expect_json("${sbom}" "MIT" components 0 licenses 0 license id)
expect_json("${sbom}" "required" components 0 scope)
expect_json("${sbom}" "pkg:generic/alpha@1.4.0" components 0 purl)
expect_json("${sbom}" "https://example.invalid/alpha"
  components 0 externalReferences 0 url)
expect_json("${sbom}" "Says \"hello\" \\ a|b" components 0 description)
expect_json("${sbom}" "checker" components 1 name)
expect_json("${sbom}" "excluded" components 1 scope)

file(READ "${REPORT_DIR}/licence-report.md" report)
expect_contains("${report}"
  "| alpha | 1.4.0 | MIT | product | Says \"hello\" \\ a\\|b | https://example.invalid/alpha |")
expect_contains("${report}" "| checker | 2.0 | BSD-3-Clause | test |")
