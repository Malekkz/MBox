scada_add_library(scada-driver
  SOURCES
    src/raw_value.cpp
    src/sample_processing.cpp
    src/utf8.cpp
    src/write_conversion.cpp
    src/zone_table.cpp
  DEPENDS scada::common scada::tag)

# Scaling and value conversion give the same bits on both architectures only
# without floating-point contraction (decision D-98). GCC leaves it off in
# ISO mode, Clang does not; state it for both.
target_compile_options(libscada-driver PRIVATE -ffp-contract=off)

if(BUILD_TESTING)
  add_subdirectory(tests)
  add_subdirectory(fuzz)
endif()
