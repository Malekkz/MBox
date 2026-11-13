# The unit test framework: GoogleTest (docs/decisions.md, D-10).
#
# It is built from source inside this build tree, never taken from the
# machine, so that it is compiled with the same sanitizer and architecture as
# the code under test.
#
# The archive is downloaded at configure time and checked against the hash
# below. To build without network access, unpack the same release somewhere
# and pass -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<directory>.

include(FetchContent)

if(POLICY CMP0135)
  # Give extracted files the time of extraction, not the time in the archive.
  cmake_policy(SET CMP0135 NEW)
endif()

set(SCADA_GOOGLETEST_VERSION 1.17.0)

FetchContent_Declare(googletest
  URL "https://github.com/google/googletest/releases/download/v${SCADA_GOOGLETEST_VERSION}/googletest-${SCADA_GOOGLETEST_VERSION}.tar.gz"
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c)

set(BUILD_GMOCK OFF CACHE BOOL "GoogleMock is not used" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "Test code is not installed" FORCE)

FetchContent_MakeAvailable(googletest)

# Our warning policy is for our code. Treating the framework's headers as
# system headers keeps its macros from failing our -Werror build.
foreach(target IN ITEMS gtest gtest_main)
  get_target_property(scada_gtest_includes ${target}
    INTERFACE_INCLUDE_DIRECTORIES)
  set_target_properties(${target} PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${scada_gtest_includes}")
endforeach()
