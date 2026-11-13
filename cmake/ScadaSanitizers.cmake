# Sanitizer and fuzzing instrumentation (docs/coding-standard.md, section 13).
#
# SCADA_SANITIZER selects one instrumented build:
#   address-undefined  AddressSanitizer with leak detection, plus
#                      UndefinedBehaviorSanitizer
#   thread             ThreadSanitizer
#   fuzz               libFuzzer coverage with AddressSanitizer and
#                      UndefinedBehaviorSanitizer; needs Clang
#
# Unlike the warning flags, these are set for the whole build tree: a
# sanitizer only gives complete reports when every object in the process is
# instrumented, and that includes the test framework.

include(CheckCXXSourceCompiles)

set(SCADA_SANITIZER "" CACHE STRING
  "Instrumented build: empty, address-undefined, thread or fuzz")
set_property(CACHE SCADA_SANITIZER PROPERTY STRINGS
  "" address-undefined thread fuzz)

set(SCADA_FUZZ_SECONDS 60 CACHE STRING
  "How long ctest runs each fuzz target, in seconds")

# Environment for every test registered through the helpers in
# ScadaTargets.cmake. Set here so that the options do not depend on how ctest
# was started.
set(SCADA_TEST_ENVIRONMENT)

if(SCADA_SANITIZER STREQUAL "")
  return()
endif()

if(SCADA_HARDENING)
  # Fortified sources replace the library calls that AddressSanitizer
  # intercepts, so overflows through them would go unreported.
  message(FATAL_ERROR
    "SCADA_SANITIZER and SCADA_HARDENING cannot be combined.")
endif()

# Any finding ends the process with a failure; nothing is only printed.
set(scada_address_undefined_flags
  -fsanitize=address,undefined -fno-sanitize-recover=all)

if(SCADA_SANITIZER STREQUAL "address-undefined")
  set(scada_sanitizer_flags ${scada_address_undefined_flags})
elseif(SCADA_SANITIZER STREQUAL "thread")
  set(scada_sanitizer_flags -fsanitize=thread)
elseif(SCADA_SANITIZER STREQUAL "fuzz")
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
      "SCADA_SANITIZER=fuzz needs Clang (libFuzzer), got "
      "'${CMAKE_CXX_COMPILER_ID}'. Use the `fuzz` preset.")
  endif()
  # Coverage instrumentation everywhere; only fuzz targets link the fuzzing
  # engine itself (scada_add_fuzz_target).
  set(scada_sanitizer_flags
    -fsanitize=fuzzer-no-link ${scada_address_undefined_flags})
else()
  message(FATAL_ERROR
    "Unknown SCADA_SANITIZER '${SCADA_SANITIZER}'. "
    "Use address-undefined, thread or fuzz.")
endif()

# Frame pointers keep the stack traces in the reports complete.
list(APPEND scada_sanitizer_flags -fno-omit-frame-pointer)

# The compiler accepts these flags even when the runtime library is not
# installed; only a link shows that.
string(MAKE_C_IDENTIFIER "SCADA_HAVE_SANITIZER_${SCADA_SANITIZER}"
  scada_sanitizer_check)
list(JOIN scada_sanitizer_flags " " CMAKE_REQUIRED_FLAGS)
check_cxx_source_compiles("int main() { return 0; }" ${scada_sanitizer_check})
unset(CMAKE_REQUIRED_FLAGS)
if(NOT ${scada_sanitizer_check})
  message(FATAL_ERROR
    "This toolchain cannot build with '${scada_sanitizer_flags}'. "
    "The sanitizer runtime libraries are probably not installed.")
endif()

add_compile_options(${scada_sanitizer_flags})
add_link_options(${scada_sanitizer_flags})

if(SCADA_SANITIZER STREQUAL "thread")
  list(APPEND SCADA_TEST_ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
else()
  list(APPEND SCADA_TEST_ENVIRONMENT
    "ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1"
    "UBSAN_OPTIONS=print_stacktrace=1")
endif()
