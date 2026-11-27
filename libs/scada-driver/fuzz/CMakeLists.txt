scada_add_fuzz_target(sample_processing_fuzz
  SOURCES sample_processing_fuzz.cpp
  DEPENDS scada::driver)

scada_add_fuzz_target(tzif_fuzz
  SOURCES tzif_fuzz.cpp
  DEPENDS scada::driver)

scada_add_fuzz_target(text_pattern_fuzz
  SOURCES text_pattern_fuzz.cpp
  DEPENDS scada::driver)

scada_add_fuzz_target(identity_text_fuzz
  SOURCES identity_text_fuzz.cpp
  DEPENDS scada::driver)
