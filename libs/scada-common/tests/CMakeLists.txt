scada_add_test(scada-common-unit-test KIND unit
  SOURCES realtime_test.cpp
  DEPENDS scada::common)
