# The simulator writer is a test tool, not part of the product image: it is
# installed only with the component `test-tools`, which a plain
# `cmake --install` leaves out (decision D-41):
#   cmake --install build/dev --component test-tools --prefix <directory>
add_executable(scada-sim src/main.cpp)
target_link_libraries(scada-sim PRIVATE scada::sim)
scada_target_defaults(scada-sim)
install(TARGETS scada-sim
  RUNTIME DESTINATION "${SCADA_INSTALL_BINDIR}"
  COMPONENT test-tools
  EXCLUDE_FROM_ALL)
