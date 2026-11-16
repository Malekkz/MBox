# Code shared by tests. Not installed.

# Starts real processes for integration tests, traces the system calls of
# one, and makes temporary directories that a killed test does not leave
# behind.
scada_add_library(scada-test-support
  SOURCES src/child_process.cpp src/exec.cpp src/syscall_trace.cpp
          src/temp_directory.cpp)

# The cleaner of TempDirectory. The library starts it by the command below:
# the emulator of a cross build, if any, then the program, elements
# separated by `|`.
add_executable(scada-test-cleaner src/cleaner_main.cpp)
scada_target_defaults(scada-test-cleaner)
set(cleaner_command ${CMAKE_CROSSCOMPILING_EMULATOR}
  "$<TARGET_FILE:scada-test-cleaner>")
list(JOIN cleaner_command "|" cleaner_command)
target_compile_definitions(libscada-test-support PRIVATE
  "SCADA_TEST_CLEANER_COMMAND=\"${cleaner_command}\"")
add_dependencies(libscada-test-support scada-test-cleaner)

# main() for fuzz targets in builds without libFuzzer; see
# scada_add_fuzz_target.
scada_add_library(scada-fuzz-replay SOURCES src/fuzz_replay_main.cpp)
