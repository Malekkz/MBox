# libscada-conf: validation of the configuration tree (configuration schemas
# contract, docs/contracts/config-schemas.md; issue #100).

# The JSON Schema files of contract Annex C, built into the library. The
# list is explicit so that the build does not depend on what else lies in
# the directory.
set(scada_conf_schemas
  common.schema.json
  point.schema.json
  system.schema.json
  areas.schema.json
  enums.schema.json
  logging.schema.json
  tasks.schema.json
  template.schema.json
  channel.schema.json
  modbus.schema.json
  io.schema.json
  tags.schema.json
  outputs.schema.json
  alarms.schema.json
  program.schema.json)
set(scada_conf_schema_entries "")
foreach(schema IN LISTS scada_conf_schemas)
  set(schema_path "${CMAKE_CURRENT_SOURCE_DIR}/schemas/${schema}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${schema_path}")
  file(READ "${schema_path}" schema_text)
  string(FIND "${schema_text}" ")SCADA_SCHEMA" delimiter_found)
  if(NOT delimiter_found EQUAL -1)
    message(FATAL_ERROR "${schema} holds the raw string delimiter")
  endif()
  string(APPEND scada_conf_schema_entries
    "    {\"${schema}\", R\"SCADA_SCHEMA(${schema_text})SCADA_SCHEMA\"},\n")
endforeach()
configure_file(src/builtin_schemas.cpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/builtin_schemas.cpp" @ONLY)

scada_add_library(scada-conf
  SOURCES
    src/channel_rules.cpp
    src/config_eval.cpp
    src/diagnostic.cpp
    src/expand.cpp
    src/expression.cpp
    src/expression_site.cpp
    src/expression_types.cpp
    src/grouped_errors.cpp
    src/merged_points.cpp
    src/modbus_rules.cpp
    src/names.cpp
    src/output_rules.cpp
    src/output_values.cpp
    src/node.cpp
    src/point_rules.cpp
    src/point_types.cpp
    src/name_pattern.cpp
    src/path_class.cpp
    src/read.cpp
    src/references.cpp
    src/rules.cpp
    src/runtime_expressions.cpp
    src/scalar.cpp
    src/schema.cpp
    src/tag_names.cpp
    src/tag_rules.cpp
    src/task_rules.cpp
    src/tree_scan.cpp
    src/utf8.cpp
    src/validate.cpp
    src/yaml_parser.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/builtin_schemas.cpp"
  DEPENDS scada::common)

if(BUILD_TESTING)
  add_subdirectory(tests)
  add_subdirectory(fuzz)
endif()
