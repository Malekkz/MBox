# The staged install tree (reference 4.4 and 14.1).
#
# `cmake --install <build> --prefix <stage>` writes an image of the target's
# root filesystem under <stage>: <stage>/usr/bin, <stage>/etc/scada and so on.
# The install prefix is therefore the staging root, not `/usr`, and the
# destinations below are relative to the root. Packaging is a thin layer over
# this tree.
#
# The paths a service sees at runtime are fixed by reference 4.4. They are the
# destinations below with a leading `/`, whatever the staging prefix is.

set(SCADA_INSTALL_BINDIR "usr/bin")
set(SCADA_INSTALL_LIBEXECDIR "usr/lib/scada")
set(SCADA_INSTALL_WEBDIR "usr/share/scada/web")
set(SCADA_INSTALL_CONFDIR "etc/scada")
set(SCADA_INSTALL_STATEDIR "var/lib/scada")
set(SCADA_INSTALL_STATE_SUBDIRS config-history hist journal retain)

# Where systemd looks for units and user definitions shipped by the vendor.
# These are systemd's own default locations; a distribution that builds
# systemd with other locations overrides them here.
set(SCADA_INSTALL_SYSTEMD_UNITDIR "usr/lib/systemd/system" CACHE STRING
  "Directory for systemd units, relative to the root of the staged tree")
set(SCADA_INSTALL_SYSUSERSDIR "usr/lib/sysusers.d" CACHE STRING
  "Directory for sysusers.d files, relative to the root of the staged tree")

# The group every service belongs to (reference 4.2).
set(SCADA_GROUP "scada")

set(SCADA_SERVICE_UNIT_TEMPLATE
  "${CMAKE_CURRENT_LIST_DIR}/../deploy/systemd/scada-service.service.in")
set(SCADA_SERVICE_SYSUSERS_TEMPLATE
  "${CMAKE_CURRENT_LIST_DIR}/../deploy/sysusers.d/scada-service.conf.in")

# scada_install_service(<name> DESCRIPTION <text> WATCHDOG_SEC <seconds>)
#
# Generates and installs what systemd needs to run the executable <name> as a
# service (reference 4.2): the unit <name>.service from the common template,
# and the definition of the system user <name>, member of the `scada` group.
# The service must report readiness and feed the watchdog (libscada-service).
function(scada_install_service name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "DESCRIPTION;WATCHDOG_SEC" "")
  if(arg_UNPARSED_ARGUMENTS OR NOT arg_DESCRIPTION
     OR NOT arg_WATCHDOG_SEC MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "scada_install_service(${name}): bad arguments")
  endif()
  if(NOT TARGET ${name})
    message(FATAL_ERROR "scada_install_service(${name}): no such executable")
  endif()

  set(SCADA_SERVICE_NAME "${name}")
  set(SCADA_SERVICE_DESCRIPTION "${arg_DESCRIPTION}")
  set(SCADA_SERVICE_WATCHDOG_SEC "${arg_WATCHDOG_SEC}")
  set(SCADA_SERVICE_EXECUTABLE "/${SCADA_INSTALL_BINDIR}/${name}")

  set(unit "${CMAKE_CURRENT_BINARY_DIR}/${name}.service")
  configure_file("${SCADA_SERVICE_UNIT_TEMPLATE}" "${unit}" @ONLY)
  install(FILES "${unit}" DESTINATION "${SCADA_INSTALL_SYSTEMD_UNITDIR}")

  set(sysusers "${CMAKE_CURRENT_BINARY_DIR}/sysusers.d/${name}.conf")
  configure_file("${SCADA_SERVICE_SYSUSERS_TEMPLATE}" "${sysusers}" @ONLY)
  install(FILES "${sysusers}" DESTINATION "${SCADA_INSTALL_SYSUSERSDIR}")

  # Lets the install test verify every unit without a list kept by hand.
  set_property(GLOBAL APPEND PROPERTY SCADA_SERVICE_UNITS "${name}.service")
endfunction()
