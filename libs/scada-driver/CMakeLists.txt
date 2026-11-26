# The directory of the compiled time zone files (TZif, RFC 9636) that
# device local times are converted with (decision D-130). The environment
# variable TZDIR overrides it at run time. The default is the directory
# POSIX systems and the tz database's own install use; a target image must
# carry the zone files there or set this option.
set(SCADA_ZONEINFO_DIR "/usr/share/zoneinfo" CACHE PATH
  "Directory of the compiled time zone files")

scada_add_library(scada-driver
  SOURCES
    src/channel_engine.cpp
    src/diagnostics.cpp
    src/driver.cpp
    src/failure.cpp
    src/fd_io.cpp
    src/plan.cpp
    src/point_rules.cpp
    src/raw_value.cpp
    src/sample_processing.cpp
    src/scheduler.cpp
    src/serial_transport.cpp
    src/stall_watch.cpp
    src/system_clock.cpp
    src/system_tags.cpp
    src/tag_writer.cpp
    src/tcp_transport.cpp
    src/tzif.cpp
    src/utf8.cpp
    src/worker.cpp
    src/write_conversion.cpp
    src/zone_file.cpp
    src/zone_table.cpp
  DEPENDS scada::common scada::conf scada::service scada::tag)

# A worker thread's stack (driver interface contract 2.4): 256 KiB, more
# under the sanitizers, whose instrumentation needs it.
if(SCADA_SANITIZER STREQUAL "")
  set(SCADA_DRIVER_WORKER_STACK_BYTES 262144)
else()
  set(SCADA_DRIVER_WORKER_STACK_BYTES 8388608)
endif()

target_compile_definitions(libscada-driver PRIVATE
  "SCADA_ZONEINFO_DIR=\"${SCADA_ZONEINFO_DIR}\""
  "SCADA_DRIVER_WORKER_STACK_BYTES=${SCADA_DRIVER_WORKER_STACK_BYTES}")

# Scaling and value conversion give the same bits on both architectures only
# without floating-point contraction (decision D-98): GCC contracts a
# multiply and an add into one fused instruction in C++ wherever the target
# has one (ARM64), whatever the language standard.
target_compile_options(libscada-driver PRIVATE -ffp-contract=off)

if(BUILD_TESTING)
  add_subdirectory(tests)
  add_subdirectory(fuzz)
endif()
