# Tests of ScadaDependencies.cmake. Each configures the fixture project with
# one dependency list from cases/ and checks that the configuration fails
# with the right message, or succeeds and writes the right files.
#
# The allow-list is the real one, so these tests also prove what it rejects,
# unless a case names its own with ALLOWLIST.

set(scada_real_allowlist
  "${PROJECT_SOURCE_DIR}/third-party/licence-allowlist.json")

# scada_add_dependency_case(<name> LIST <case> [LINKS <names...>]
#   [USES_VERSION_OF <dependency>] [EXPECT_ERROR <regex>]
#   [CHECK_SCRIPT <file>] [ALLOWLIST <case>])
#
# In LINKS, write a generator expression as "@<...>": add_test would evaluate
# "$<...>" itself. run_case.cmake turns it back.
function(scada_add_dependency_case name)
  cmake_parse_arguments(PARSE_ARGV 1 arg ""
    "LIST;EXPECT_ERROR;CHECK_SCRIPT;USES_VERSION_OF;ALLOWLIST" "LINKS")
  list(JOIN arg_LINKS "|" links)
  set(allowlist "${scada_real_allowlist}")
  if(DEFINED arg_ALLOWLIST)
    set(allowlist
      "${CMAKE_CURRENT_SOURCE_DIR}/cases/${arg_ALLOWLIST}/licence-allowlist.json")
  endif()
  set(arguments
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixture"
    "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/${name}"
    "-DGENERATOR=${CMAKE_GENERATOR}"
    "-DMODULE_DIR=${PROJECT_SOURCE_DIR}/cmake"
    "-DLIST=${CMAKE_CURRENT_SOURCE_DIR}/cases/${arg_LIST}/dependencies.json"
    "-DALLOWLIST=${allowlist}"
    "-DLINKS=${links}"
    "-DUSES_VERSION_OF=${arg_USES_VERSION_OF}")
  if(DEFINED arg_EXPECT_ERROR)
    list(APPEND arguments "-DEXPECT_ERROR=${arg_EXPECT_ERROR}")
  endif()
  if(DEFINED arg_CHECK_SCRIPT)
    list(APPEND arguments
      "-DCHECK_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/${arg_CHECK_SCRIPT}")
  endif()
  add_test(NAME dependency-check-${name}
    COMMAND "${CMAKE_COMMAND}" ${arguments}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_case.cmake")
  set_tests_properties(dependency-check-${name} PROPERTIES
    LABELS unit TIMEOUT 60)
endfunction()

# Accepted.
scada_add_dependency_case(empty-list LIST empty CHECK_SCRIPT check_empty.cmake)
scada_add_dependency_case(listed LIST listed
  LINKS external::alpha alpha-raw Threads::Threads m
  CHECK_SCRIPT check_listed.cmake)
scada_add_dependency_case(listed-link-only LIST listed
  LINKS "@<LINK_ONLY:external::alpha>")

# Rejected: something is linked that is not in the list.
scada_add_dependency_case(unlisted-target LIST listed
  LINKS external::alpha external::beta
  EXPECT_ERROR "target 'own' links 'external::beta', which is not in the dependency list")
scada_add_dependency_case(unlisted-target-empty-list LIST empty
  LINKS external::alpha
  EXPECT_ERROR "target 'own' links 'external::alpha', which is not in the dependency list")
scada_add_dependency_case(unlisted-library-name LIST listed LINKS z
  EXPECT_ERROR "target 'own' links 'z', which is not in the dependency list")
scada_add_dependency_case(unlisted-link-only LIST listed
  LINKS "@<LINK_ONLY:external::beta>"
  EXPECT_ERROR "target 'own' links 'external::beta', which is not in the dependency list")
scada_add_dependency_case(unlisted-library-path LIST listed
  LINKS /somewhere/libbeta.a
  EXPECT_ERROR "target 'own' links '/somewhere/libbeta.a', which is not in the dependency list")
scada_add_dependency_case(uncheckable-expression LIST listed
  LINKS "@<TARGET_NAME_IF_EXISTS:external::beta>"
  EXPECT_ERROR "which cannot be checked")
scada_add_dependency_case(unlisted-vendored-directory LIST vendored
  EXPECT_ERROR "directory 'beta' next to the list is not a listed dependency")

# Rejected: the licence is not on the allow-list.
scada_add_dependency_case(gpl-licence LIST gpl
  EXPECT_ERROR "dependency 'alpha': licence 'GPL-3.0-only' is not on the allow-list")
scada_add_dependency_case(licence-expression LIST licence-expression
  EXPECT_ERROR "licence 'MIT OR GPL-3.0-only' is not on the allow-list")

# A licence allowed for some scopes only (CECILL-B: build and test, D-37).
scada_add_dependency_case(limited-licence-product LIST limited-licence-product
  EXPECT_ERROR "dependency 'alpha': licence 'CECILL-B' is allowed for scope build, test only, not 'product'")
scada_add_dependency_case(limited-licence-test LIST limited-licence-test
  CHECK_SCRIPT check_limited.cmake)
scada_add_dependency_case(bad-licence-scopes LIST empty
  ALLOWLIST bad-licence-scopes
  EXPECT_ERROR "\"licence_scopes\" of 'MIT': scope 'runtime' must be product, build or test.*\"licence_scopes\" of 'Unlisted-1.0': the licence is not allowed")
scada_add_dependency_case(bad-commit LIST bad-commit
  EXPECT_ERROR "dependency 'alpha': the commit must be a hexadecimal hash in lower case")
add_test(NAME dependency-check-allowlist-has-no-gpl
  COMMAND "${CMAKE_COMMAND}" "-DALLOWLIST=${scada_real_allowlist}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_allowlist.cmake")
set_tests_properties(dependency-check-allowlist-has-no-gpl PROPERTIES
  LABELS unit TIMEOUT 60)

scada_add_dependency_case(fetched-but-unlisted LIST empty
  USES_VERSION_OF alpha
  EXPECT_ERROR "dependency 'alpha' is used by the build but is not in the dependency list")
scada_add_dependency_case(fetched-and-listed LIST listed USES_VERSION_OF alpha)

# Rejected: the list is malformed.
scada_add_dependency_case(missing-version LIST missing-version
  EXPECT_ERROR "entry 0: \"version\" is missing")
scada_add_dependency_case(misspelt-key LIST misspelt-key
  EXPECT_ERROR "entry 0: unknown key \"license\"")
scada_add_dependency_case(duplicate-name LIST duplicate
  EXPECT_ERROR "dependency 'alpha': listed more than once")
scada_add_dependency_case(bad-scope LIST bad-scope
  EXPECT_ERROR "scope 'runtime' must be product, build or test")
scada_add_dependency_case(malformed-json LIST malformed
  EXPECT_ERROR "the file is not valid JSON")
scada_add_dependency_case(future-schema LIST future-schema
  EXPECT_ERROR "the file has schema_version '2'")
