scada_add_fuzz_target(toy_parser_fuzz
  SOURCES toy_parser_fuzz.cpp
  DEPENDS scada::toy-parser
  EXPECT_FINDING "AddressSanitizer: heap-buffer-overflow")
