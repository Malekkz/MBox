# The directory of the compiled time zone files (TZif, RFC 9636) that
# device local times are converted with (decision D-130). The environment
# variable TZDIR overrides it at run time. The default is the directory
# POSIX systems and the tz database's own install use; a target image must
# carry the zone files there or set this option.
set(SCADA_ZONEINFO_DIR "/usr/share/zoneinfo" CACHE PATH
  "Directory of the compiled time zone files")

scada_add_library(scada-driver
  SOURCES
    src/point_rules.cpp
    src/raw_value.cpp
    src/sample_processing.cpp
    src/tzif.cpp
    src/utf8.cpp
    src/write_conversion.cpp
    src/zone_table.cpp
  DEPENDS scada::common scada::tag)

target_compile_definitions(libscada-driver PRIVATE
  "SCADA_ZONEINFO_DIR=\"${SCADA_ZONEINFO_DIR}\"")

# Scaling and value conversion give the same bits on both architectures only
# without floating-point contraction (decision D-98). GCC leaves it off in
# ISO mode, Clang does not; state it for both.
target_compile_options(libscada-driver PRIVATE -ffp-contract=off)

if(BUILD_TESTING)
  add_subdirectory(tests)
  add_subdirectory(fuzz)
endif()
