# Compile and link options shared by every target in this repository. The
# flags are those of docs/coding-standard.md, section 2.
#
# They are attached per target (see ScadaTargets.cmake) instead of being set
# globally, so that third-party code added later is not built with our
# warning policy.
#
# A flag the toolchain does not support stops the configuration. The coding
# standard forbids dropping a flag silently: the substitute must be chosen,
# recorded in docs/decisions.md and added here.

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)
include(CheckPIESupported)

option(SCADA_HARDENING
  "Add the release flags of the coding standard, section 2" OFF)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
  message(FATAL_ERROR
    "Unsupported compiler '${CMAKE_CXX_COMPILER_ID}'. Use GCC or Clang.")
endif()

# All builds, both architectures.
set(SCADA_WARNING_FLAGS
  -Wall -Wextra -Wpedantic -Werror
  -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast -Wcast-align
  -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference
  -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
set(SCADA_COMMON_DEFINITIONS _GLIBCXX_ASSERTIONS)

# Release builds add these. Position-independent executables (-fPIE -pie) are
# requested through CMake's POSITION_INDEPENDENT_CODE instead.
set(SCADA_HARDENING_COMPILE_FLAGS
  -fstack-protector-strong
  -fstack-clash-protection
  # Some toolchains predefine _FORTIFY_SOURCE; undefine first so that the
  # redefinition is not a warning, and therefore not an error.
  -U_FORTIFY_SOURCE
  -D_FORTIFY_SOURCE=3)
set(SCADA_HARDENING_LINK_FLAGS -Wl,-z,relro,-z,now -Wl,-z,noexecstack)

# The only architecture-specific flags: control-flow protection.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  list(APPEND SCADA_HARDENING_COMPILE_FLAGS -fcf-protection=full)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  list(APPEND SCADA_HARDENING_COMPILE_FLAGS -mbranch-protection=standard)
else()
  message(FATAL_ERROR
    "Unsupported architecture '${CMAKE_SYSTEM_PROCESSOR}'. "
    "Supported: x86-64 and ARM64 (reference 4.1).")
endif()

# Stops the configuration if the compiler rejects one of `flags`.
function(scada_require_compile_flags)
  foreach(flag IN LISTS ARGN)
    if(flag MATCHES "^-[DU]")
      continue()
    endif()
    string(MAKE_C_IDENTIFIER "SCADA_HAVE${flag}" result_variable)
    check_cxx_compiler_flag("${flag}" ${result_variable})
    if(NOT ${result_variable})
      message(FATAL_ERROR
        "The compiler does not support '${flag}', which "
        "docs/coding-standard.md section 2 requires. Record a substitute in "
        "docs/decisions.md; do not drop the flag.")
    endif()
  endforeach()
endfunction()

scada_require_compile_flags(${SCADA_WARNING_FLAGS})

if(SCADA_HARDENING)
  # Fortified sources have no effect without optimisation, so a hardened
  # build that is not optimised would silently miss one of the measures.
  if(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    message(FATAL_ERROR
      "SCADA_HARDENING needs an optimised build type, got "
      "'${CMAKE_BUILD_TYPE}'.")
  endif()

  scada_require_compile_flags(${SCADA_HARDENING_COMPILE_FLAGS})

  # Fortify level 3 relies on this builtin. A compiler without it would accept
  # -D_FORTIFY_SOURCE=3 and quietly give level 2.
  check_cxx_source_compiles("
    int main(int argc, char* argv[]) {
      return static_cast<int>(__builtin_dynamic_object_size(argv[0], 0)) + argc;
    }" SCADA_HAVE_FORTIFY_LEVEL_3)
  if(NOT SCADA_HAVE_FORTIFY_LEVEL_3)
    message(FATAL_ERROR
      "The compiler cannot provide _FORTIFY_SOURCE=3. Record the substitute "
      "in docs/decisions.md; do not lower the level silently.")
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
  target_compile_definitions(${target} PRIVATE ${SCADA_COMMON_DEFINITIONS})
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
