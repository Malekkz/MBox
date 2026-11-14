# Conformance test 4: the model check of the sequence lock and the change
# ring for the Armv8 memory model, and the check that the ARM64 library
# still compiles to what the model describes. See README.md in this
# directory and docs/decisions.md, D-34.
#
# The interpreter is named by its path, not by its imported target: these
# are tools of the build machine, and CTest would run an imported target
# under the emulator of a cross build.

find_package(Python3 3.8 COMPONENTS Interpreter)

option(SCADA_REQUIRE_HERD7
  "Stop the configuration if herd7 is not found (the model-check job)" OFF)
find_program(SCADA_HERD7 herd7 DOC "Path of herd7 (herdtools7)")

if(NOT Python3_Interpreter_FOUND)
  if(SCADA_REQUIRE_HERD7)
    message(FATAL_ERROR "The model check needs Python 3.8 or newer.")
  endif()
  message(WARNING "Python 3.8 or newer not found: no model check tests.")
  return()
endif()

# The checker of the disassembly, on a fixture; runs on any host.
add_test(NAME scada-model-code-shape-self-test
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/code_shape_test.py")
set_tests_properties(scada-model-code-shape-self-test
  PROPERTIES LABELS unit TIMEOUT 60)

# The model check. herd7 is not a build requirement; without it the test is
# not registered, except in the model-check job, which requires it.
if(SCADA_HERD7)
  add_test(NAME scada-model-check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/model_check.py"
      --herd7 "${SCADA_HERD7}")
  # herd7 runs 10 times, two tests with four weakened copies each: about
  # 275 s on the pipeline's x86-64 runner, most of it the seqlock test.
  set_tests_properties(scada-model-check PROPERTIES LABELS model TIMEOUT 600)
elseif(SCADA_REQUIRE_HERD7)
  message(FATAL_ERROR
    "herd7 not found; set SCADA_HERD7 to its path (tests/model/README.md).")
else()
  message(STATUS
    "herd7 not found: the model check (conformance test 4) is not "
    "registered (tests/model/README.md).")
endif()

# The code the model describes exists only in an ARM64 build with
# optimisation: at -O0 the atomic accesses are calls, and a sanitizer adds
# its own. CMAKE_OBJDUMP is the objdump of the toolchain, so this works in a
# cross build too.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$"
    AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug"
    AND SCADA_SANITIZER STREQUAL "")
  if(NOT CMAKE_OBJDUMP)
    message(FATAL_ERROR "objdump of the toolchain not found (CMAKE_OBJDUMP).")
  endif()
  add_test(NAME scada-model-code-shape-test
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/code_shape.py"
      --objdump "${CMAKE_OBJDUMP}" "$<TARGET_FILE:libscada-tag>")
  set_tests_properties(scada-model-code-shape-test
    PROPERTIES LABELS unit TIMEOUT 60)
endif()
