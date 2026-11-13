scada_add_library(scada-service
  SOURCES src/notify.cpp src/notify_listener.cpp src/unix_address.cpp
  DEPENDS scada::common)

if(BUILD_TESTING)
  add_subdirectory(tests)
  add_subdirectory(fuzz)
endif()
