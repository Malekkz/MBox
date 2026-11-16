scada_add_executable(scada-tagd SOURCES src/main.cpp DEPENDS scada::tagd)
scada_install_service(scada-tagd
  DESCRIPTION "MBox tag database daemon"
  WATCHDOG_SEC 10)

# The platform creates the base directory of the tag database at boot (tag
# database contract, section 3.1); scada-tagd does not.
install(FILES "${PROJECT_SOURCE_DIR}/deploy/tmpfiles.d/scada-tagd.conf"
  DESTINATION "${SCADA_INSTALL_TMPFILESDIR}")

# Section 12: scada-tagd writes the retained values, in a file system the
# unit makes read-only otherwise.
install(FILES "${PROJECT_SOURCE_DIR}/deploy/systemd/scada-tagd.service.d/retain.conf"
  DESTINATION "${SCADA_INSTALL_SYSTEMD_UNITDIR}/scada-tagd.service.d")

# Decision D-59: the stop of scada-tagd is bounded (5 s for the retain
# writer, then an exit at once); systemd waits 15 s for it.
install(FILES "${PROJECT_SOURCE_DIR}/deploy/systemd/scada-tagd.service.d/stop.conf"
  DESTINATION "${SCADA_INSTALL_SYSTEMD_UNITDIR}/scada-tagd.service.d")
