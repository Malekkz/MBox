# Installs the build tree BUILD_DIR into a fresh staged tree at STAGE.

file(REMOVE_RECURSE "${STAGE}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${STAGE}"
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "cmake --install failed: ${result}")
endif()
