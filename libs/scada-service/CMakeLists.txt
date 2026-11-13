scada_add_library(scada-service
  SOURCES src/fd.cpp src/notify.cpp src/notify_listener.cpp src/unix_address.cpp)

if(BUILD_TESTING)
  add_subdirectory(tests)
  add_subdirectory(fuzz)
endif()
