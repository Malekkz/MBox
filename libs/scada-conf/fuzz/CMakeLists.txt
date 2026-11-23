# Path names come from outside (an imported archive, reference 12.7): the
# classifier of stage 1 is fuzzed (CLAUDE.md, every parser of outside input).
scada_add_fuzz_target(path_class_fuzz
  SOURCES path_class_fuzz.cpp
  DEPENDS scada::conf)
