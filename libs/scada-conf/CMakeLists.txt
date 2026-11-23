# libscada-conf: validation of the configuration tree (configuration schemas
# contract, docs/contracts/config-schemas.md; issue #100).
scada_add_library(scada-conf
  SOURCES
    src/diagnostic.cpp
    src/node.cpp
    src/path_class.cpp
    src/tree_scan.cpp
    src/validate.cpp
    src/yaml_parser.cpp
  DEPENDS scada::common)

if(BUILD_TESTING)
  add_subdirectory(tests)
  add_subdirectory(fuzz)
endif()
