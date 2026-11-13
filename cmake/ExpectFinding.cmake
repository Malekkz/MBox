# Runs a command that must fail with a given report, for tests that prove a
# checker catches a deliberate bug. A plain CTest regular expression is not
# enough: it would accept a report from a process that still exits with
# success, which a real test run would count as a pass.
#
#   cmake -DSCADA_COMMAND=<program>|<argument>|...
#         -DSCADA_REGEX=<regular expression the output must match>
#         [-DSCADA_FRESH_DIRS=<directory>|...]   emptied before the run
#         -P ExpectFinding.cmake
#
# List elements are separated by `|` because `;` does not survive a command
# line.

if(NOT SCADA_COMMAND OR NOT SCADA_REGEX)
  message(FATAL_ERROR "SCADA_COMMAND and SCADA_REGEX are required.")
endif()

string(REPLACE "|" ";" command "${SCADA_COMMAND}")
string(REPLACE "|" ";" fresh_dirs "${SCADA_FRESH_DIRS}")

foreach(dir IN LISTS fresh_dirs)
  file(REMOVE_RECURSE "${dir}")
  file(MAKE_DIRECTORY "${dir}")
endforeach()

execute_process(
  COMMAND ${command}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output)

message("${output}")

if(result STREQUAL "0")
  message(FATAL_ERROR
    "Expected a finding matching '${SCADA_REGEX}', but the command exited "
    "with success.")
endif()
if(NOT output MATCHES "${SCADA_REGEX}")
  message(FATAL_ERROR
    "The command failed (${result}), but its output does not match "
    "'${SCADA_REGEX}'.")
endif()
message("Finding reported as expected: ${SCADA_REGEX}")
