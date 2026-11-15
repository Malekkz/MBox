scada_add_test(scadactl-unit-test KIND unit
  SOURCES format_test.cpp pattern_test.cpp
  DEPENDS scada::scadactl)
