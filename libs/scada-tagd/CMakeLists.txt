scada_add_library(scada-tagd
  SOURCES src/late_log.cpp src/report_log.cpp src/tagd.cpp
  DEPENDS scada::service scada::tag)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
