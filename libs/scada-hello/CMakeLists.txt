scada_add_library(scada-hello SOURCES src/hello.cpp)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
