# Tests that span several components, and what they share. Unit tests live in
# the tests/ directory of their component. See README.md for the layout.
if(NOT BUILD_TESTING)
  return()
endif()

add_subdirectory(support)
add_subdirectory(integration)
add_subdirectory(install)
add_subdirectory(perf)
add_subdirectory(model)
add_subdirectory(fuzz-sample)
add_subdirectory(sanitizer-fixtures)
