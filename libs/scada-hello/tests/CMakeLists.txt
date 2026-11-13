scada_add_test(scada-hello-unit-test KIND unit
  SOURCES hello_test.cpp
  DEPENDS scada::hello)
