scada_add_test(scada-conf-unit-test KIND unit
  SOURCES
    config_eval_test.cpp
    corpus_test.cpp
    diagnostic_test.cpp
    expand_test.cpp
    merged_points_test.cpp
    point_rules_test.cpp
    expression_test.cpp
    expression_types_test.cpp
    names_test.cpp
    node_test.cpp
    path_class_test.cpp
    read_test.cpp
    references_test.cpp
    rules_test.cpp
    scalar_test.cpp
    runtime_expressions_test.cpp
    tag_names_test.cpp
    tag_rules_test.cpp
    tag_names_prefix_test.cpp
    schema_examples_test.cpp
    schema_test.cpp
    tree_scan_test.cpp
    yaml_test.cpp
  DEPENDS scada::conf scada::test-support)
# The conformance corpus of contract section 14, read in place.
target_compile_definitions(scada-conf-unit-test PRIVATE
  "SCADA_CONF_CORPUS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/corpus\"")
# expression_types.h: the type checker, not part of the public interface.
target_include_directories(scada-conf-unit-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# The default limits of the YAML parser at their edges, a file of 8 MiB or a
# million nodes, and the cost of the schema check of typed keys: each a time
# limit of its own, apart from the unit tests above (#164).
scada_add_test(scada-conf-yaml-limits-test KIND unit
  SOURCES yaml_limits_test.cpp
  DEPENDS scada::conf scada::test-support)
scada_add_test(scada-conf-schema-limits-test KIND unit
  SOURCES schema_limits_test.cpp
  DEPENDS scada::conf scada::test-support)

# The limits of stage 4 at their edges: millions of nodes each, so each has
# a time limit of its own, apart from the unit tests above.
scada_add_test(scada-conf-limits-test KIND unit
  SOURCES limits_test.cpp
  DEPENDS scada::conf scada::test-support)
scada_add_test(scada-conf-checked-values-limits-test KIND unit
  SOURCES checked_values_limits_test.cpp
  DEPENDS scada::conf scada::test-support)
scada_add_test(scada-conf-tree-limits-test KIND unit
  SOURCES tree_limits_test.cpp
  DEPENDS scada::conf scada::test-support)
scada_add_test(scada-conf-expression-limits-test KIND unit
  SOURCES expression_limits_test.cpp
  DEPENDS scada::conf scada::test-support)
# The work of the run-time expressions of stage 5 at tens of thousands of
# tags and patterns (D-117), with its own time limit.
scada_add_test(scada-conf-runtime-limits-test KIND unit
  SOURCES runtime_limits_test.cpp
  DEPENDS scada::conf scada::test-support)
# The work of the health rules' patterns of stage 5 at tens of thousands of
# points (D-120), with its own time limit.
scada_add_test(scada-conf-point-limits-test KIND unit
  SOURCES point_limits_test.cpp
  DEPENDS scada::conf scada::test-support)
