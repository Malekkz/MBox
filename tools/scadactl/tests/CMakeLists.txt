scada_add_test(scadactl-unit-test KIND unit
  SOURCES arguments_test.cpp format_test.cpp pattern_test.cpp view_test.cpp
  DEPENDS scada::scadactl)
