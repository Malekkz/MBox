# Tests of the staged install tree: it is installed into the build directory,
# its layout is checked against reference 4.4, and systemd checks every unit.

set(stage "${CMAKE_CURRENT_BINARY_DIR}/stage")
get_property(units GLOBAL PROPERTY SCADA_SERVICE_UNITS)

add_test(NAME install-stage
  COMMAND "${CMAKE_COMMAND}"
    -DBUILD_DIR=${CMAKE_BINARY_DIR} -DSTAGE=${stage}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/install_stage.cmake")
set_tests_properties(install-stage PROPERTIES FIXTURES_SETUP staged-tree)

add_test(NAME install-layout
  COMMAND "${CMAKE_COMMAND}"
    -DSTAGE=${stage}
    -DBINDIR=${SCADA_INSTALL_BINDIR}
    -DUNITDIR=${SCADA_INSTALL_SYSTEMD_UNITDIR}
    -DSYSUSERSDIR=${SCADA_INSTALL_SYSUSERSDIR}
    -DTMPFILESDIR=${SCADA_INSTALL_TMPFILESDIR}
    "-DUNITS=${units}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_layout.cmake")
set_tests_properties(install-layout PROPERTIES FIXTURES_REQUIRED staged-tree)

# systemd-analyze is needed to run this test. Without it the test fails; it
# is not skipped, because an unchecked unit must not pass for a checked one.
add_test(NAME install-units-verify
  COMMAND "${CMAKE_COMMAND}"
    -DSTAGE=${stage}
    -DUNITDIR=${SCADA_INSTALL_SYSTEMD_UNITDIR}
    "-DUNITS=${units}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_units.cmake")
set_tests_properties(install-units-verify PROPERTIES
  FIXTURES_REQUIRED staged-tree)

# The component `test-tools` installs the simulator writer on its own.
add_test(NAME install-test-tools
  COMMAND "${CMAKE_COMMAND}"
    -DBUILD_DIR=${CMAKE_BINARY_DIR}
    -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/test-tools
    -DBINDIR=${SCADA_INSTALL_BINDIR}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/install_test_tools.cmake")

set_tests_properties(install-stage install-layout install-units-verify
  install-test-tools
  PROPERTIES
    LABELS integration
    TIMEOUT 300
    ENVIRONMENT "${SCADA_TEST_ENVIRONMENT}")
