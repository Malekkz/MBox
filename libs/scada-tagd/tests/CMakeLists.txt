scada_add_test(scada-tagd-unit-test KIND unit
  SOURCES tagd_test.cpp suspend_test.cpp
  DEPENDS scada::tagd)
