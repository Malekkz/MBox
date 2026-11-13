scada_add_executable(scada-hello SOURCES src/main.cpp DEPENDS scada::hello)
scada_install_service(scada-hello
  DESCRIPTION "MBox build and service skeleton check"
  ARGUMENTS --service
  WATCHDOG_SEC 10)
