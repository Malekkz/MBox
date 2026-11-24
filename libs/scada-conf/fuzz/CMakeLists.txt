# Path names come from outside (an imported archive, reference 12.7): the
# classifier of stage 1 is fuzzed (CLAUDE.md, every parser of outside input).
scada_add_fuzz_target(path_class_fuzz
  SOURCES path_class_fuzz.cpp
  DEPENDS scada::conf)

# The YAML subset parser of stage 2. Its seed corpus holds the examples of the
# contract and every construct the subset refuses.
scada_add_fuzz_target(yaml_fuzz
  SOURCES yaml_fuzz.cpp
  DEPENDS scada::conf)
