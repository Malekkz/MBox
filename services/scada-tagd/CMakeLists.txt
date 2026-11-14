scada_add_executable(scada-tagd SOURCES src/main.cpp DEPENDS scada::tagd)
scada_install_service(scada-tagd
  DESCRIPTION "MBox tag database daemon"
  WATCHDOG_SEC 10)

# The platform creates the base directory of the tag database at boot (tag
# database contract, section 3.1); scada-tagd does not.
install(FILES "${PROJECT_SOURCE_DIR}/deploy/tmpfiles.d/scada-tagd.conf"
  DESTINATION "${SCADA_INSTALL_TMPFILESDIR}")
