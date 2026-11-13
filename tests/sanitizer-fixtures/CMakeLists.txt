# Deliberate bugs that prove each sanitizer build catches its class of error.
# They exist only in the sanitizer builds: the program has undefined
# behaviour by design and must not be built, installed or run anywhere else.
if(NOT SCADA_SANITIZER MATCHES "^(address-undefined|thread)$")
  return()
endif()

add_executable(scada-sanitizer-fixture fixture.cpp)
scada_target_defaults(scada-sanitizer-fixture)

# scada_expect_sanitizer_report(<bug> <regex>): the fixture run with <bug>
# must fail with a report matching <regex>.
function(scada_expect_sanitizer_report bug regex)
  add_test(NAME sanitizer-catches-${bug} COMMAND "${CMAKE_COMMAND}"
    "-DSCADA_COMMAND=$<TARGET_FILE:scada-sanitizer-fixture>|${bug}"
    "-DSCADA_REGEX=${regex}"
    -P "${PROJECT_SOURCE_DIR}/cmake/ExpectFinding.cmake")
  set_tests_properties(sanitizer-catches-${bug} PROPERTIES
    LABELS sanitizer
    TIMEOUT 60
    ENVIRONMENT "${SCADA_TEST_ENVIRONMENT}")
endfunction()

if(SCADA_SANITIZER STREQUAL "address-undefined")
  scada_expect_sanitizer_report(heap-overflow
    "AddressSanitizer: heap-buffer-overflow")
  scada_expect_sanitizer_report(use-after-free
    "AddressSanitizer: heap-use-after-free")
  scada_expect_sanitizer_report(leak
    "LeakSanitizer: detected memory leaks")
  scada_expect_sanitizer_report(signed-overflow
    "runtime error: signed integer overflow")
else()
  scada_expect_sanitizer_report(data-race
    "ThreadSanitizer: data race")
endif()
