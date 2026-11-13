# Compile and link options shared by every target in this repository.
#
# They are attached per target (see ScadaTargets.cmake) instead of being set
# globally, so that third-party code added later is not built with our
# warning policy.

include(CheckPIESupported)

option(SCADA_HARDENING "Enable the hardening flags of reference 13.6" OFF)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
  message(FATAL_ERROR
    "Unsupported compiler '${CMAKE_CXX_COMPILER_ID}'. Use GCC or Clang.")
endif()

# Warnings are errors (CLAUDE.md, Code).
set(SCADA_WARNING_FLAGS -Wall -Wextra -Wpedantic -Werror)

# Reference 13.6: stack protection, position-independent executables, full
# RELRO, fortified sources. None of these is architecture-specific.
set(SCADA_HARDENING_COMPILE_FLAGS
  -fstack-protector-strong
  # Some toolchains predefine _FORTIFY_SOURCE; undefine first so that the
  # redefinition is not a warning, and therefore not an error.
  -U_FORTIFY_SOURCE
  -D_FORTIFY_SOURCE=2)
set(SCADA_HARDENING_LINK_FLAGS -Wl,-z,relro -Wl,-z,now)

if(SCADA_HARDENING)
  # Fortified sources have no effect without optimisation, so a hardened
  # build that is not optimised would silently miss one of the four measures.
  if(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    message(FATAL_ERROR
      "SCADA_HARDENING needs an optimised build type, got "
      "'${CMAKE_BUILD_TYPE}'.")
  endif()
  check_pie_supported(OUTPUT_VARIABLE scada_pie_output LANGUAGES CXX)
  if(NOT CMAKE_CXX_LINK_PIE_SUPPORTED)
    message(FATAL_ERROR
      "SCADA_HARDENING needs position-independent executables, which this "
      "toolchain cannot link: ${scada_pie_output}")
  endif()
endif()

# Applies the common options to one target.
function(scada_target_defaults target)
  target_compile_options(${target} PRIVATE ${SCADA_WARNING_FLAGS})
  if(SCADA_HARDENING)
    target_compile_options(${target} PRIVATE ${SCADA_HARDENING_COMPILE_FLAGS})
    get_target_property(scada_target_type ${target} TYPE)
    if(NOT scada_target_type STREQUAL "STATIC_LIBRARY")
      target_link_options(${target} PRIVATE ${SCADA_HARDENING_LINK_FLAGS})
    endif()
    # Libraries must be position-independent too, to link into a PIE.
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()
endfunction()
