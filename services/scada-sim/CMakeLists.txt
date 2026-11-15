# The simulator writer: installed as a tool, without a service unit; it is
# started by hand or by a test, never at boot (decision D-38).
scada_add_executable(scada-sim SOURCES src/main.cpp DEPENDS scada::sim)
