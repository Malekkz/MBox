scada_add_fuzz_target(journal_stream_fuzz
  SOURCES journal_stream_fuzz.cpp
  DEPENDS scada::common)
# log_internal.h: the parser is not part of the public interface.
target_include_directories(journal_stream_fuzz
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
