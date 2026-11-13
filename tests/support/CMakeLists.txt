# Code shared by tests. Not installed.

# Starts real processes for integration tests.
scada_add_library(scada-test-support SOURCES src/child_process.cpp)

# main() for fuzz targets in builds without libFuzzer; see
# scada_add_fuzz_target.
scada_add_library(scada-fuzz-replay SOURCES src/fuzz_replay_main.cpp)
