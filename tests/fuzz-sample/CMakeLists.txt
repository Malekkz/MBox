# Sample fuzz target. The toy parser contains a planted bug and must never be
# linked into product code; it exists to prove that the fuzz build finds bugs.
scada_add_library(scada-toy-parser SOURCES src/toy_parser.cpp)

add_subdirectory(fuzz)
