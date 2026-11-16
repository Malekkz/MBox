# Checks the staged tree at STAGE against the directory layout of reference
# 4.4. BINDIR, UNITDIR, SYSUSERSDIR and TMPFILESDIR are relative to STAGE;
# UNITS is the list of service units.

set(failures 0)

function(expect_directory path)
  if(NOT IS_DIRECTORY "${STAGE}/${path}")
    message(SEND_ERROR "missing directory: ${path}")
  endif()
endfunction()

function(expect_file path)
  if(NOT EXISTS "${STAGE}/${path}" OR IS_DIRECTORY "${STAGE}/${path}")
    message(SEND_ERROR "missing file: ${path}")
  endif()
endfunction()

# Reference 4.4. /run/scada and /dev/shm/scada exist only at runtime.
expect_directory("etc/scada")
expect_directory("var/lib/scada/config-history")
expect_directory("var/lib/scada/hist")
expect_directory("var/lib/scada/journal")
expect_directory("var/lib/scada/retain")
expect_directory("usr/lib/scada")
expect_directory("usr/bin")
expect_file("usr/bin/scadactl")
expect_directory("usr/share/scada/web")

# Licence report and bill of materials (reference 13.6, 16.3).
expect_file("usr/share/doc/scada/licence-report.md")
expect_file("usr/share/doc/scada/sbom.cdx.json")

expect_file("${SYSUSERSDIR}/scada.conf")
if(NOT UNITS)
  message(SEND_ERROR "no service units are installed")
endif()
foreach(unit IN LISTS UNITS)
  string(REGEX REPLACE "\\.service$" "" service "${unit}")
  expect_file("${BINDIR}/${service}")
  expect_file("${UNITDIR}/${unit}")
  expect_file("${SYSUSERSDIR}/${service}.conf")
endforeach()

# Test tools are not part of the product image (decision D-41).
foreach(tool IN ITEMS scada-sim)
  if(EXISTS "${STAGE}/${BINDIR}/${tool}")
    message(SEND_ERROR "test tool in the staged tree: ${BINDIR}/${tool}")
  endif()
endforeach()

# The base directory of the tag database is created at boot with the owner
# and mode of the tag database contract, section 3.1.
expect_file("${TMPFILESDIR}/scada-tagd.conf")
if(EXISTS "${STAGE}/${TMPFILESDIR}/scada-tagd.conf")
  file(STRINGS "${STAGE}/${TMPFILESDIR}/scada-tagd.conf" tmpfiles_lines
    REGEX "^[^#]")
  set(expected_tmpfiles
    "d /dev/shm/scada 2750 scada-tagd scada -"
    "d /var/lib/scada/retain 2750 scada-tagd scada -")
  if(NOT tmpfiles_lines STREQUAL expected_tmpfiles)
    message(SEND_ERROR
      "scada-tagd.conf does not create /dev/shm/scada and /var/lib/scada/retain as the contract requires: ${tmpfiles_lines}")
  endif()
endif()

# scada-tagd may write the retain directory (contract, section 12).
expect_file("${UNITDIR}/scada-tagd.service.d/retain.conf")
if(EXISTS "${STAGE}/${UNITDIR}/scada-tagd.service.d/retain.conf")
  file(STRINGS "${STAGE}/${UNITDIR}/scada-tagd.service.d/retain.conf"
    retain_lines REGEX "^ReadWritePaths=")
  if(NOT retain_lines STREQUAL "ReadWritePaths=/var/lib/scada/retain")
    message(SEND_ERROR "the drop-in of scada-tagd does not allow writing /var/lib/scada/retain")
  endif()
endif()

# scada-tagd needs no capability beyond its own user and the scada group
# (issue P1-04).
if(EXISTS "${STAGE}/${UNITDIR}/scada-tagd.service")
  file(STRINGS "${STAGE}/${UNITDIR}/scada-tagd.service" tagd_unit)
  foreach(line IN ITEMS
      "User=scada-tagd" "Group=scada-tagd" "SupplementaryGroups=scada"
      "CapabilityBoundingSet=" "AmbientCapabilities=" "NoNewPrivileges=yes")
    list(FIND tagd_unit "${line}" position)
    if(position EQUAL -1)
      message(SEND_ERROR "scada-tagd.service lacks the line: ${line}")
    endif()
  endforeach()
  foreach(line IN LISTS tagd_unit)
    if(line MATCHES "^(PrivateIPC|PrivateUsers|DynamicUser)=")
      # Tag database contract, section 3.4.
      message(SEND_ERROR "scada-tagd.service must not set: ${line}")
    endif()
  endforeach()
endif()

# Nothing may be installed outside the layout.
file(GLOB top_level RELATIVE "${STAGE}" "${STAGE}/*")
foreach(entry IN LISTS top_level)
  if(NOT entry MATCHES "^(etc|usr|var)$")
    message(SEND_ERROR "unexpected top-level entry: ${entry}")
  endif()
endforeach()
