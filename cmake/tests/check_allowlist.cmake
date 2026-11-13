# The allow-list of this repository must not contain a GPL licence
# (CLAUDE.md, Safety and security rules; reference 16).
#
#   cmake -DALLOWLIST=... -P check_allowlist.cmake

file(READ "${ALLOWLIST}" json)
string(JSON count LENGTH "${json}" allowed_licences)
if(count EQUAL 0)
  message(FATAL_ERROR "The allow-list is empty.")
endif()
math(EXPR last "${count} - 1")
foreach(index RANGE ${last})
  string(JSON licence GET "${json}" allowed_licences ${index})
  if(licence MATCHES "^A?GPL")
    message(FATAL_ERROR "'${licence}' is on the allow-list.")
  endif()
endforeach()
