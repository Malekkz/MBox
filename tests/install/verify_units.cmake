# Runs `systemd-analyze verify` on each unit in UNITS, found in UNITDIR of the
# staged tree at STAGE. The staged tree is used as the root, so the check also
# covers that the executable named in the unit is installed.
#
# The staged tree holds no operating system, so the units ours refer to
# (sysinit.target and the like) are not in it. `--recursive-errors=no` limits
# the check to the unit itself; it needs systemd 250 or newer.

find_program(SYSTEMD_ANALYZE systemd-analyze)
if(NOT SYSTEMD_ANALYZE)
  message(FATAL_ERROR
    "systemd-analyze was not found. It is needed to verify the units.")
endif()
if(NOT UNITS)
  message(FATAL_ERROR "no service units to verify")
endif()

foreach(unit IN LISTS UNITS)
  execute_process(
    COMMAND "${SYSTEMD_ANALYZE}" verify "--root=${STAGE}" --recursive-errors=no
      "${STAGE}/${UNITDIR}/${unit}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
  # systemd-analyze reports some problems as text with exit code 0, so any
  # output is a failure too.
  if(NOT result EQUAL 0 OR NOT output STREQUAL "")
    message(SEND_ERROR "${unit}: systemd-analyze verify failed:\n${output}")
  else()
    message(STATUS "${unit}: verified")
  endif()
endforeach()
