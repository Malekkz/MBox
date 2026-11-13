# Parts of the staged tree that belong to no single component.

install(FILES sysusers.d/scada.conf DESTINATION "${SCADA_INSTALL_SYSUSERSDIR}")

# The directories of reference 4.4 that are on disk. They are installed empty
# and owned by root; each is handed to its service's user by the issue that
# adds the service. /run/scada and /dev/shm/scada are on memory-backed
# filesystems and are created at runtime by the services that own them.
install(DIRECTORY DESTINATION "${SCADA_INSTALL_CONFDIR}")
install(DIRECTORY DESTINATION "${SCADA_INSTALL_LIBEXECDIR}")
install(DIRECTORY DESTINATION "${SCADA_INSTALL_WEBDIR}")
foreach(subdir IN LISTS SCADA_INSTALL_STATE_SUBDIRS)
  install(DIRECTORY DESTINATION "${SCADA_INSTALL_STATEDIR}/${subdir}")
endforeach()
