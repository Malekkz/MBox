# Helpers for the check scripts.

# Fails unless the JSON value at the given path equals `expected`.
function(expect_json json expected)
  string(JSON actual ERROR_VARIABLE error GET "${json}" ${ARGN})
  if(error OR NOT actual STREQUAL expected)
    message(FATAL_ERROR
      "Bill of materials: ${ARGN} is '${actual}', expected '${expected}'. "
      "${error}")
  endif()
endfunction()

# Fails unless `text` contains `needle`.
function(expect_contains text needle)
  string(FIND "${text}" "${needle}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Expected to find '${needle}' in:\n${text}")
  endif()
endfunction()
