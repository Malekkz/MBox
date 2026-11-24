scada_add_test(scada-conf-unit-test KIND unit
  SOURCES
    corpus_test.cpp
    diagnostic_test.cpp
    names_test.cpp
    node_test.cpp
    path_class_test.cpp
    read_test.cpp
    scalar_test.cpp
    schema_test.cpp
    tree_scan_test.cpp
    yaml_test.cpp
  DEPENDS scada::conf scada::test-support)
# The conformance corpus of contract section 14, read in place.
target_compile_definitions(scada-conf-unit-test PRIVATE
  "SCADA_CONF_CORPUS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/corpus\"")
