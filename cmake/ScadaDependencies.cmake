# Third-party dependency list, licence check, licence report and software bill
# of materials (reference 13.6 and 16.3). See third-party/README.md.
#
# Everything runs at configure time, because that is when the link graph is
# known. The two input files are configure dependencies, so editing one makes
# the next build configure again and fail if the list is no longer valid.

include_guard(GLOBAL)

# Link names that belong to the C library of the platform, not to a third
# party. They need no entry in the dependency list (decision D-21).
set(SCADA_PLATFORM_LINK_NAMES Threads::Threads pthread m dl rt atomic)

set(_SCADA_DEPENDENCY_KEYS
  name version licence scope purpose url link_names purl commit)
set(_SCADA_DEPENDENCY_REQUIRED_KEYS name version licence scope purpose url)
set(_SCADA_DEPENDENCY_SCOPES product build test)

# Stops the configuration with every problem found, one per indented line.
# Indented lines are not re-wrapped by CMake, so they stay searchable.
function(_scada_dependency_fail list_file)
  # A failed check must not leave output behind that looks valid.
  get_property(output_files GLOBAL PROPERTY SCADA_DEPENDENCY_OUTPUT_FILES)
  if(output_files)
    file(REMOVE ${output_files})
  endif()
  list(JOIN ARGN "\n  " problems)
  message(FATAL_ERROR
    "Dependency check failed:\n\n  ${problems}\n\n"
    "Every third-party dependency must be listed in '${list_file}' with its "
    "version and a licence from the allow-list. See third-party/README.md.")
endfunction()

# Reads `file` into `out` and checks that it is a JSON object with
# "schema_version": 1.
function(_scada_read_json_file out file)
  if(NOT EXISTS "${file}")
    _scada_dependency_fail("${file}" "the file does not exist")
  endif()
  file(READ "${file}" text)
  string(JSON schema_version ERROR_VARIABLE error GET "${text}" schema_version)
  if(error)
    _scada_dependency_fail("${file}"
      "the file is not valid JSON or has no \"schema_version\"")
  endif()
  if(NOT schema_version STREQUAL "1")
    _scada_dependency_fail("${file}"
      "the file has schema_version '${schema_version}', this build understands 1")
  endif()
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

# Reads the array of strings `json[key]` into the list `out`. Problems are
# appended to the list named by `problems_var`; `where` names the place.
function(_scada_json_string_array out json key where problems_var)
  set(values "")
  set(problems "${${problems_var}}")
  string(JSON type ERROR_VARIABLE error TYPE "${json}" "${key}")
  if(error OR NOT type STREQUAL "ARRAY")
    list(APPEND problems "${where}: \"${key}\" must be an array of strings")
  else()
    string(JSON count LENGTH "${json}" "${key}")
    if(count GREATER 0)
      math(EXPR last "${count} - 1")
      foreach(index RANGE ${last})
        string(JSON type TYPE "${json}" "${key}" ${index})
        string(JSON value GET "${json}" "${key}" ${index})
        if(NOT type STREQUAL "STRING" OR value STREQUAL ""
           OR value MATCHES "[;\n\r\t]")
          list(APPEND problems
            "${where}: \"${key}\"[${index}] must be a non-empty string "
            "without ';' or control characters")
        else()
          list(APPEND values "${value}")
        endif()
      endforeach()
    endif()
  endif()
  set(${out} "${values}" PARENT_SCOPE)
  set(${problems_var} "${problems}" PARENT_SCOPE)
endfunction()

# Quotes `text` as a JSON string. Control characters are rejected earlier.
function(_scada_json_quote out text)
  string(REPLACE "\\" "\\\\" text "${text}")
  string(REPLACE "\"" "\\\"" text "${text}")
  set(${out} "\"${text}\"" PARENT_SCOPE)
endfunction()

# scada_dependencies(LIST <file> ALLOWLIST <file> OUTPUT_DIR <dir>)
#
# Call once, from the top-level CMakeLists.txt, before any target is created.
# Stops the configuration if
#   - an entry of LIST is malformed or its licence is not in ALLOWLIST,
#   - an entry's licence is limited to some scopes in ALLOWLIST
#     ("licence_scopes") and the entry has another scope,
#   - a directory next to LIST (vendored code) is not named after an entry,
#   - at the end of the configuration, a target of this project links
#     something that is neither a target of this project, nor a link name of
#     an entry, nor a platform library.
# Writes <dir>/licence-report.md and <dir>/sbom.cdx.json (CycloneDX 1.5).
function(scada_dependencies)
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "LIST;ALLOWLIST;OUTPUT_DIR" "")
  if(arg_UNPARSED_ARGUMENTS OR NOT arg_LIST OR NOT arg_ALLOWLIST
     OR NOT arg_OUTPUT_DIR)
    message(FATAL_ERROR "scada_dependencies: bad arguments")
  endif()
  if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR
      "scada_dependencies must be called from the top-level CMakeLists.txt")
  endif()

  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${arg_LIST}" "${arg_ALLOWLIST}")

  # A failed check must not leave earlier output behind that looks valid.
  set(report_file "${arg_OUTPUT_DIR}/licence-report.md")
  set(sbom_file "${arg_OUTPUT_DIR}/sbom.cdx.json")
  file(REMOVE "${report_file}" "${sbom_file}")
  set_property(GLOBAL PROPERTY SCADA_DEPENDENCY_OUTPUT_FILES
    "${report_file}" "${sbom_file}")

  set(problems "")

  _scada_read_json_file(allowlist_json "${arg_ALLOWLIST}")
  _scada_json_string_array(allowed_licences "${allowlist_json}"
    allowed_licences "allow-list" problems)

  # Optional "licence_scopes": a licence allowed for some scopes only, as
  # {"<licence>": ["build", "test"]}. A licence not named there is allowed
  # for every scope.
  set(limited_licences "")
  string(JSON type ERROR_VARIABLE error TYPE "${allowlist_json}"
    licence_scopes)
  if(NOT error)
    if(NOT type STREQUAL "OBJECT")
      list(APPEND problems "allow-list: \"licence_scopes\" must be an object")
    else()
      string(JSON limit_count LENGTH "${allowlist_json}" licence_scopes)
      if(limit_count GREATER 0)
        math(EXPR limit_last "${limit_count} - 1")
        foreach(limit_index RANGE ${limit_last})
          string(JSON limited MEMBER "${allowlist_json}" licence_scopes
            ${limit_index})
          string(JSON limit_json GET "${allowlist_json}" licence_scopes)
          set(where "allow-list: \"licence_scopes\" of '${limited}'")
          _scada_json_string_array(scopes "${limit_json}" "${limited}"
            "${where}" problems)
          if(NOT limited IN_LIST allowed_licences)
            list(APPEND problems "${where}: the licence is not allowed")
          endif()
          if(NOT scopes)
            list(APPEND problems "${where}: names no scope")
          endif()
          foreach(scope IN LISTS scopes)
            if(NOT scope IN_LIST _SCADA_DEPENDENCY_SCOPES)
              list(APPEND problems
                "${where}: scope '${scope}' must be product, build or test")
            endif()
          endforeach()
          list(APPEND limited_licences "${limited}")
          set(scopes_of_${limited} "${scopes}")
        endforeach()
      endif()
    endif()
  endif()

  _scada_read_json_file(list_json "${arg_LIST}")
  string(JSON type ERROR_VARIABLE error TYPE "${list_json}" dependencies)
  if(error OR NOT type STREQUAL "ARRAY")
    _scada_dependency_fail("${arg_LIST}"
      "\"dependencies\" must be an array" ${problems})
  endif()

  set(names "")
  set(all_link_names "")
  set(components "[]")
  set(report_rows "")

  string(JSON count LENGTH "${list_json}" dependencies)
  if(count GREATER 0)
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
      set(where "entry ${index}")
      string(JSON entry GET "${list_json}" dependencies ${index})
      string(JSON type TYPE "${list_json}" dependencies ${index})
      if(NOT type STREQUAL "OBJECT")
        list(APPEND problems "${where}: must be an object")
        continue()
      endif()

      # Read the members; an unknown key is an error, so that a misspelt
      # optional key is not ignored silently.
      foreach(key IN LISTS _SCADA_DEPENDENCY_KEYS)
        unset(field_${key})
      endforeach()
      set(entry_ok TRUE)
      string(JSON member_count LENGTH "${entry}")
      if(member_count GREATER 0)
        math(EXPR member_last "${member_count} - 1")
        foreach(member_index RANGE ${member_last})
          string(JSON key MEMBER "${entry}" ${member_index})
          if(NOT key IN_LIST _SCADA_DEPENDENCY_KEYS)
            list(APPEND problems "${where}: unknown key \"${key}\"")
            set(entry_ok FALSE)
          elseif(key STREQUAL "link_names")
            _scada_json_string_array(field_link_names "${entry}" link_names
              "${where}" problems)
          else()
            string(JSON type TYPE "${entry}" "${key}")
            string(JSON value GET "${entry}" "${key}")
            if(NOT type STREQUAL "STRING" OR value STREQUAL ""
               OR value MATCHES "[;\n\r\t]")
              list(APPEND problems
                "${where}: \"${key}\" must be a non-empty string without "
                "';' or control characters")
              set(entry_ok FALSE)
            else()
              set(field_${key} "${value}")
            endif()
          endif()
        endforeach()
      endif()
      foreach(key IN LISTS _SCADA_DEPENDENCY_REQUIRED_KEYS)
        if(NOT DEFINED field_${key})
          list(APPEND problems "${where}: \"${key}\" is missing")
          set(entry_ok FALSE)
        endif()
      endforeach()
      if(NOT entry_ok)
        continue()
      endif()

      set(where "dependency '${field_name}'")
      if(NOT field_name MATCHES "^[A-Za-z0-9][A-Za-z0-9._+-]*$")
        list(APPEND problems
          "${where}: the name may contain only letters, digits and . _ + -")
      endif()
      if(field_name IN_LIST names)
        list(APPEND problems "${where}: listed more than once")
      endif()
      if(NOT field_scope IN_LIST _SCADA_DEPENDENCY_SCOPES)
        list(APPEND problems
          "${where}: scope '${field_scope}' must be product, build or test")
      endif()
      if(NOT field_licence IN_LIST allowed_licences)
        list(APPEND problems
          "${where}: licence '${field_licence}' is not on the allow-list")
      elseif(field_licence IN_LIST limited_licences
             AND NOT field_scope IN_LIST scopes_of_${field_licence})
        list(JOIN scopes_of_${field_licence} ", " scopes_text)
        list(APPEND problems
          "${where}: licence '${field_licence}' is allowed for scope ${scopes_text} only, not '${field_scope}'")
      endif()
      if(DEFINED field_commit)
        # CMake regular expressions have no {n}: the length is checked apart.
        string(LENGTH "${field_commit}" commit_length)
        if(NOT field_commit MATCHES "^[0-9a-f]+$" OR NOT commit_length EQUAL 40)
          list(APPEND problems
            "${where}: the commit must be 40 hexadecimal characters in lower case")
        endif()
      endif()
      list(APPEND names "${field_name}")
      set_property(GLOBAL PROPERTY
        "SCADA_DEPENDENCY_VERSION_${field_name}" "${field_version}")
      list(APPEND all_link_names ${field_link_names})

      # Bill of materials component. Only `product` dependencies ship.
      if(field_scope STREQUAL "product")
        set(sbom_scope "required")
      else()
        set(sbom_scope "excluded")
      endif()
      _scada_json_quote(q_ref "${field_name}@${field_version}")
      _scada_json_quote(q_name "${field_name}")
      _scada_json_quote(q_version "${field_version}")
      _scada_json_quote(q_licence "${field_licence}")
      _scada_json_quote(q_purpose "${field_purpose}")
      _scada_json_quote(q_url "${field_url}")
      set(component "{
        \"type\": \"library\",
        \"bom-ref\": ${q_ref},
        \"name\": ${q_name},
        \"version\": ${q_version},
        \"description\": ${q_purpose},
        \"scope\": \"${sbom_scope}\",
        \"licenses\": [{\"license\": {\"id\": ${q_licence}}}],
        \"externalReferences\": [{\"type\": \"website\", \"url\": ${q_url}}]
      }")
      if(DEFINED field_purl)
        _scada_json_quote(q_purl "${field_purl}")
        string(JSON component SET "${component}" purl "${q_purl}")
      endif()
      string(JSON component_count LENGTH "${components}")
      string(JSON components SET "${components}" ${component_count}
        "${component}")

      string(REPLACE "|" "\\|" purpose_cell "${field_purpose}")
      string(APPEND report_rows
        "| ${field_name} | ${field_version} | ${field_licence} "
        "| ${field_scope} | ${purpose_cell} | ${field_url} |\n")
    endforeach()
  endif()

  # Vendored code: every directory next to the list is a dependency.
  get_filename_component(vendor_dir "${arg_LIST}" DIRECTORY)
  file(GLOB vendored LIST_DIRECTORIES true RELATIVE "${vendor_dir}"
    "${vendor_dir}/*")
  foreach(item IN LISTS vendored)
    if(IS_DIRECTORY "${vendor_dir}/${item}" AND NOT item IN_LIST names)
      list(APPEND problems
        "directory '${item}' next to the list is not a listed dependency")
    endif()
  endforeach()

  if(problems)
    _scada_dependency_fail("${arg_LIST}" ${problems})
  endif()

  # Licence report.
  set(allowed_text_items "")
  foreach(licence IN LISTS allowed_licences)
    if(licence IN_LIST limited_licences)
      list(JOIN scopes_of_${licence} " and " scopes_text)
      list(APPEND allowed_text_items "${licence} (scope ${scopes_text} only)")
    else()
      list(APPEND allowed_text_items "${licence}")
    endif()
  endforeach()
  list(JOIN allowed_text_items ", " allowed_text)
  set(report "# Licence report\n\n")
  string(APPEND report
    "Project: ${PROJECT_NAME} ${PROJECT_VERSION}\n\n"
    "Generated by the build from `third-party/dependencies.json`. "
    "Do not edit.\n\n")
  if(report_rows)
    string(APPEND report
      "| Name | Version | Licence | Scope | Purpose | URL |\n"
      "| --- | --- | --- | --- | --- | --- |\n"
      "${report_rows}")
  else()
    string(APPEND report "No third-party dependencies.\n")
  endif()
  string(APPEND report "\nAllowed licences: ${allowed_text}\n")
  file(WRITE "${report_file}" "${report}")

  # Bill of materials. No timestamp or serial number: the same input gives
  # the same file.
  _scada_json_quote(q_project "${PROJECT_NAME}")
  _scada_json_quote(q_project_version "${PROJECT_VERSION}")
  set(sbom "{
    \"bomFormat\": \"CycloneDX\",
    \"specVersion\": \"1.5\",
    \"version\": 1,
    \"metadata\": {
      \"component\": {
        \"type\": \"application\",
        \"bom-ref\": ${q_project},
        \"name\": ${q_project},
        \"version\": ${q_project_version}
      }
    }
  }")
  string(JSON sbom SET "${sbom}" components "${components}")
  file(WRITE "${sbom_file}" "${sbom}\n")

  set_property(GLOBAL PROPERTY SCADA_DEPENDENCY_LIST_FILE "${arg_LIST}")
  set_property(GLOBAL PROPERTY SCADA_DEPENDENCY_VENDOR_DIR "${vendor_dir}")
  set_property(GLOBAL PROPERTY SCADA_DEPENDENCY_LINK_NAMES ${all_link_names})

  # The link graph is complete only when the top-level directory is done.
  cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
    CALL _scada_check_linked_dependencies)
endfunction()

# Appends to `out` the targets created in `dir` and below, skipping
# directories that hold third-party code: vendored directories, anything in
# the build tree (fetched sources) and anything outside the source tree.
function(_scada_collect_own_targets out dir vendor_dir)
  set(targets "${${out}}")
  foreach(excluded IN ITEMS "${vendor_dir}" "${CMAKE_BINARY_DIR}")
    string(FIND "${dir}/" "${excluded}/" position)
    if(position EQUAL 0)
      return()
    endif()
  endforeach()
  string(FIND "${dir}/" "${CMAKE_SOURCE_DIR}/" position)
  if(NOT position EQUAL 0)
    return()
  endif()

  get_property(here DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  list(APPEND targets ${here})
  get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(subdir IN LISTS subdirs)
    _scada_collect_own_targets(targets "${subdir}" "${vendor_dir}")
  endforeach()
  set(${out} "${targets}" PARENT_SCOPE)
endfunction()

# Checks everything the targets of this project link. Runs deferred.
function(_scada_check_linked_dependencies)
  get_property(list_file GLOBAL PROPERTY SCADA_DEPENDENCY_LIST_FILE)
  get_property(vendor_dir GLOBAL PROPERTY SCADA_DEPENDENCY_VENDOR_DIR)
  get_property(link_names GLOBAL PROPERTY SCADA_DEPENDENCY_LINK_NAMES)

  set(own_targets "")
  _scada_collect_own_targets(own_targets "${CMAKE_SOURCE_DIR}" "${vendor_dir}")

  set(problems "")
  foreach(target IN LISTS own_targets)
    get_target_property(direct ${target} LINK_LIBRARIES)
    get_target_property(interface ${target} INTERFACE_LINK_LIBRARIES)
    set(items "")
    foreach(property_value IN ITEMS "${direct}" "${interface}")
      if(property_value)
        list(APPEND items ${property_value})
      endif()
    endforeach()
    list(REMOVE_DUPLICATES items)

    foreach(item IN LISTS items)
      # Unwrap the generator expressions that only wrap a name.
      while(item MATCHES
          "^\\$<(LINK_ONLY|BUILD_INTERFACE|BUILD_LOCAL_INTERFACE|TARGET_NAME):(.*)>$")
        set(item "${CMAKE_MATCH_2}")
      endwhile()
      if(item STREQUAL "" OR item MATCHES "^\\$<INSTALL_INTERFACE:")
        continue()
      endif()
      if(item MATCHES "\\$<")
        list(APPEND problems
          "target '${target}' links '${item}', which cannot be checked; link a plain name")
        continue()
      endif()

      set(resolved "${item}")
      if(TARGET "${item}")
        get_target_property(aliased "${item}" ALIASED_TARGET)
        if(aliased)
          set(resolved "${aliased}")
        endif()
      endif()
      if(resolved IN_LIST own_targets
         OR item IN_LIST link_names OR resolved IN_LIST link_names
         OR item IN_LIST SCADA_PLATFORM_LINK_NAMES)
        continue()
      endif()
      list(APPEND problems
        "target '${target}' links '${item}', which is not in the dependency list")
    endforeach()
  endforeach()

  if(problems)
    _scada_dependency_fail("${list_file}" ${problems})
  endif()
endfunction()

# scada_dependency_version(<out> <name>)
#
# Sets `out` to the version of the listed dependency `name`, so that the code
# that fetches a dependency takes its version from the list instead of
# repeating it. Stops the configuration if `name` is not listed.
function(scada_dependency_version out name)
  get_property(version GLOBAL PROPERTY "SCADA_DEPENDENCY_VERSION_${name}")
  if(NOT version)
    get_property(list_file GLOBAL PROPERTY SCADA_DEPENDENCY_LIST_FILE)
    _scada_dependency_fail("${list_file}"
      "dependency '${name}' is used by the build but is not in the dependency list")
  endif()
  set(${out} "${version}" PARENT_SCOPE)
endfunction()
