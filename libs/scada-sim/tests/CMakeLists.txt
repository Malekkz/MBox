scada_add_test(scada-sim-unit-test KIND unit
  SOURCES
    layout_test.cpp
    options_test.cpp
    pattern_test.cpp
    tag_set_test.cpp
  DEPENDS scada::sim)
