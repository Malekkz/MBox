# Performance tests. They are registered in every build so that they keep
# compiling, but only `ctest --preset perf` runs them: the figures mean
# something only with the release flags.
scada_add_test(scada-hello-perf-test KIND perf
  SOURCES hello_perf_test.cpp
  DEPENDS scada::hello)
