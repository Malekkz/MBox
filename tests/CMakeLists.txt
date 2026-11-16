# Tests that span several components, and what they share. Unit tests live in
# the tests/ directory of their component. See README.md for the layout.
if(NOT BUILD_TESTING)
  return()
endif()

# Every conformance test of the tag database contract is automated and
# listed in conformance.md (issue P1-12, decision D-57).
add_test(NAME scada-conformance-index
  COMMAND "${CMAKE_COMMAND}"
    "-DCONTRACT=${PROJECT_SOURCE_DIR}/docs/contracts/tag-database.md"
    "-DINDEX=${CMAKE_CURRENT_SOURCE_DIR}/conformance.md"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/conformance_index_check.cmake")
set_tests_properties(scada-conformance-index PROPERTIES LABELS unit TIMEOUT 60)

add_subdirectory(support)
add_subdirectory(integration)
add_subdirectory(install)
add_subdirectory(perf)
add_subdirectory(model)
add_subdirectory(fuzz-sample)
add_subdirectory(sanitizer-fixtures)
