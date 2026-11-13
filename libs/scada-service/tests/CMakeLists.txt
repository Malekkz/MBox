scada_add_test(scada-service-unit-test KIND unit
  SOURCES notify_test.cpp
  DEPENDS scada::service)
