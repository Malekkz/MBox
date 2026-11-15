# Installs only the component `test-tools` of BUILD_DIR into a fresh tree at
# STAGE and checks that it holds the simulator writer and nothing of the
# product image.

file(REMOVE_RECURSE "${STAGE}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${STAGE}"
    --component test-tools
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "cmake --install --component test-tools failed: ${result}")
endif()
if(NOT EXISTS "${STAGE}/${BINDIR}/scada-sim")
  message(SEND_ERROR "missing file: ${BINDIR}/scada-sim")
endif()
file(GLOB_RECURSE installed RELATIVE "${STAGE}" "${STAGE}/*")
if(NOT installed STREQUAL "${BINDIR}/scada-sim")
  message(SEND_ERROR "the component holds more than scada-sim: ${installed}")
endif()
