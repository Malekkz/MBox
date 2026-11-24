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

# Stage 3, the schema validator, over any parsed file. Its seeds are files of
# every type with a schema, valid and not.
scada_add_fuzz_target(schema_fuzz
  SOURCES schema_fuzz.cpp
  DEPENDS scada::conf)

# The scalar text forms and the typed reads, against a second writing of the
# grammar of contract 1.2. Its seeds are the edges of each form.
scada_add_fuzz_target(scalar_fuzz
  SOURCES scalar_fuzz.cpp
  DEPENDS scada::conf)

# `validate_tree` over a small tree written to a temporary directory: every
# stage together, the rules of stage 3 included. Its seeds are the trees of
# the conformance corpus, each file as its path, a line feed and its content,
# separated by NUL bytes.
scada_add_fuzz_target(tree_fuzz
  SOURCES tree_fuzz.cpp
  DEPENDS scada::conf)
