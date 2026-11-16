# Conformance test 4: the model check of the sequence lock and the change
# ring for the Armv8 memory model, and the check that the ARM64 library
# still compiles to what the model describes. See README.md in this
# directory and docs/decisions.md, D-37.
#
# The interpreter is named by its path, not by its imported target: these
# are tools of the build machine, and CTest would run an imported target
# under the emulator of a cross build.

find_package(Python3 3.8 COMPONENTS Interpreter)

# The code the model describes exists only in an ARM64 build with
# optimisation: at -O0 the atomic accesses are calls, and a sanitizer adds
# its own. There the check of that code is required, so a missing tool stops
# the configuration instead of leaving the tests green without it. An empty
# build type adds no -O flag, so it counts as a build without optimisation.
set(scada_check_arm64_code OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$"
    AND CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$"
    AND SCADA_SANITIZER STREQUAL "")
  set(scada_check_arm64_code ON)
endif()

option(SCADA_REQUIRE_HERD7
  "Stop the configuration if herd7 is not found (the model-check job)" OFF)
find_program(SCADA_HERD7 herd7 DOC "Path of herd7 (herdtools7)")

if(NOT Python3_Interpreter_FOUND)
  if(SCADA_REQUIRE_HERD7 OR scada_check_arm64_code)
    message(FATAL_ERROR "The model check and the check of the ARM64 code "
      "need Python 3.8 or newer.")
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
# not registered, except in the model-check job, which requires it. It takes
# minutes, so only the test preset `model` runs it: the presets `dev`,
# `release` and the sanitizer presets leave the label `model` out.
if(SCADA_HERD7)
  add_test(NAME scada-model-check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/model_check.py"
      --herd7 "${SCADA_HERD7}" --timeout 300)
  # herd7 runs 22 times, three tests with four weakened copies each and the
  # handshake (D-54) with six: about 275 s on the pipeline's x86-64 runner,
  # most of it the seqlock test (44 s to 48 s a run; the repair and the
  # handshake take about a second each). The limits leave room for a slower
  # machine.
  set_tests_properties(scada-model-check PROPERTIES LABELS model TIMEOUT 900)
elseif(SCADA_REQUIRE_HERD7)
  message(FATAL_ERROR
    "herd7 not found; set SCADA_HERD7 to its path (tests/model/README.md).")
else()
  message(STATUS
    "herd7 not found: the model check (conformance test 4) is not "
    "registered (tests/model/README.md).")
endif()

# CMAKE_OBJDUMP is the objdump of the toolchain, so this works in a cross
# build too.
if(scada_check_arm64_code)
  if(NOT CMAKE_OBJDUMP)
    message(FATAL_ERROR "objdump of the toolchain not found (CMAKE_OBJDUMP).")
  endif()
  add_test(NAME scada-model-code-shape-test
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/code_shape.py"
      --objdump "${CMAKE_OBJDUMP}" "$<TARGET_FILE:libscada-tag>")
  set_tests_properties(scada-model-code-shape-test
    PROPERTIES LABELS unit TIMEOUT 60)
  # Edits of the C++ compiled with this build's compiler and flags must each
  # fail the check above (issue #50). It reads the commands of the build.
  if(NOT CMAKE_EXPORT_COMPILE_COMMANDS)
    message(FATAL_ERROR "The check of edited sources needs "
      "CMAKE_EXPORT_COMPILE_COMMANDS.")
  endif()
  add_test(NAME scada-model-code-shape-mutants-test
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/code_shape_mutants.py"
      --objdump "${CMAKE_OBJDUMP}"
      --compile-commands "${CMAKE_BINARY_DIR}/compile_commands.json")
  # 22 compilations, as many at a time as there are processors: about 10 s
  # on 4 processors.
  set_tests_properties(scada-model-code-shape-mutants-test
    PROPERTIES LABELS unit TIMEOUT 600)
endif()
