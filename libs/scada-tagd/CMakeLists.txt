scada_add_library(scada-tagd
  SOURCES src/tagd.cpp src/suspend.cpp
  DEPENDS scada::service scada::tag)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
