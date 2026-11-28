scada_add_fuzz_target(sample_processing_fuzz
  SOURCES sample_processing_fuzz.cpp
  DEPENDS scada::driver)

# The corpus must reach GOOD and UNCERTAIN writes, substitutes and holds,
# not only pass (#165): its replay fails when a quality or a path is never
# seen.
if(SCADA_SANITIZER STREQUAL "fuzz")
  set(reach_command sample_processing_fuzz -runs=0)
else()
  set(reach_command sample_processing_fuzz)
endif()
add_test(NAME sample_processing_fuzz-reach
  COMMAND ${reach_command}
    "${CMAKE_CURRENT_SOURCE_DIR}/corpus/sample_processing_fuzz")
set_tests_properties(sample_processing_fuzz-reach PROPERTIES
  LABELS unit
  TIMEOUT 60
  ENVIRONMENT "${SCADA_TEST_ENVIRONMENT};SCADA_FUZZ_REACH=1")

scada_add_fuzz_target(tzif_fuzz
  SOURCES tzif_fuzz.cpp
  DEPENDS scada::driver)

scada_add_fuzz_target(text_pattern_fuzz
  SOURCES text_pattern_fuzz.cpp
  DEPENDS scada::driver)

scada_add_fuzz_target(identity_text_fuzz
  SOURCES identity_text_fuzz.cpp
  DEPENDS scada::driver)
