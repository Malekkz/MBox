# Checks tests/conformance.md against the contract and the sources (issue
# P1-12, decision D-57). Run as a CTest test:
#
#   cmake -DCONTRACT=<contract> -DINDEX=<index> -DSOURCE_DIR=<source dir>
#         -P conformance_index_check.cmake
#
# Fails, naming each problem, when
# - section 15 of the contract does not number its tests 1, 2, 3, ...,
# - a test of section 15 has no row in the index, or a row names no test,
# - the index has a row for a test the contract does not have,
# - a name in the index is no GoogleTest test (`Suite.Name`, from TEST,
#   TEST_F or TEST_P in a .cpp file) and no CTest test (`ctest:<name>`, from
#   add_test(NAME ...) or scada_add_test(...) in a CMakeLists.txt),
# - a GoogleTest test whose suite or name begins with `Conformance<NN>` is
#   not named in row NN.

cmake_minimum_required(VERSION 3.21)

foreach(variable CONTRACT INDEX SOURCE_DIR)
  if(NOT DEFINED ${variable})
    message(FATAL_ERROR "${variable} is not set")
  endif()
endforeach()

set(problems "")
macro(problem text)
  list(APPEND problems "${text}")
endmacro()

# The tests of section 15: the numbered items between its heading and the
# next section.
file(READ "${CONTRACT}" contract)
string(FIND "${contract}" "\n## 15. Conformance tests" start)
string(FIND "${contract}" "\n## 16." end)
if(start EQUAL -1 OR end EQUAL -1 OR end LESS start)
  message(FATAL_ERROR "${CONTRACT}: no section 15 followed by section 16")
endif()
math(EXPR length "${end} - ${start}")
string(SUBSTRING "${contract}" ${start} ${length} section)
string(REGEX MATCHALL "\n[0-9]+\\. " items "${section}")
set(count 0)
foreach(item IN LISTS items)
  string(REGEX REPLACE "[^0-9]" "" number "${item}")
  math(EXPR count "${count} + 1")
  if(NOT number EQUAL count)
    problem("contract section 15: test ${number} where ${count} was expected")
  endif()
endforeach()
if(count EQUAL 0)
  message(FATAL_ERROR "${CONTRACT}: section 15 numbers no tests")
endif()

# Every GoogleTest test and every CTest test of the sources.
file(GLOB_RECURSE sources
  "${SOURCE_DIR}/libs/*.cpp" "${SOURCE_DIR}/services/*.cpp"
  "${SOURCE_DIR}/tests/*.cpp")
set(gtests "")
foreach(source IN LISTS sources)
  file(READ "${source}" text)
  string(REGEX MATCHALL
    "TEST(_F|_P)?\\([ \t\r\n]*[A-Za-z0-9_]+[ \t\r\n]*,[ \t\r\n]*[A-Za-z0-9_]+"
    found "${text}")
  foreach(match IN LISTS found)
    string(REGEX REPLACE "^TEST(_F|_P)?\\(" "" match "${match}")
    string(REGEX REPLACE "[ \t\r\n]" "" match "${match}")
    string(REPLACE "," "." match "${match}")
    list(APPEND gtests "${match}")
  endforeach()
endforeach()
file(GLOB_RECURSE listfiles
  "${SOURCE_DIR}/libs/CMakeLists.txt" "${SOURCE_DIR}/services/CMakeLists.txt"
  "${SOURCE_DIR}/tests/CMakeLists.txt")
set(ctests "")
foreach(listfile IN LISTS listfiles)
  file(READ "${listfile}" text)
  string(REGEX MATCHALL
    "(add_test\\([ \t\r\n]*NAME|scada_add_test\\()[ \t\r\n]*[A-Za-z0-9_.-]+"
    found "${text}")
  foreach(match IN LISTS found)
    string(REGEX REPLACE "^.*[( \t\r\n]" "" match "${match}")
    list(APPEND ctests "${match}")
  endforeach()
endforeach()

# The rows of the index: `| <number> | <pipeline> | <names> |`.
file(STRINGS "${INDEX}" rows REGEX "^\\| [0-9]+ \\|")
set(listed_numbers "")
foreach(row IN LISTS rows)
  string(REGEX MATCH "^\\| ([0-9]+) \\|" ignored "${row}")
  set(number "${CMAKE_MATCH_1}")
  if(number IN_LIST listed_numbers)
    problem("index: test ${number} has two rows")
  endif()
  list(APPEND listed_numbers "${number}")
  if(number LESS 1 OR number GREATER count)
    problem("index: test ${number} is not in contract section 15")
  endif()
  # The names are in the last column.
  string(REGEX REPLACE "\\|[ \t]*$" "" row "${row}")
  string(REGEX REPLACE "^.*\\|" "" names_column "${row}")
  string(REGEX MATCHALL "`[^`]+`" names "${names_column}")
  if(names STREQUAL "")
    problem("index: test ${number} names no test")
  endif()
  set(row_names_${number} "")
  foreach(name IN LISTS names)
    string(REPLACE "`" "" name "${name}")
    list(APPEND row_names_${number} "${name}")
    if(name MATCHES "^ctest:(.+)$")
      if(NOT CMAKE_MATCH_1 IN_LIST ctests)
        problem("index: test ${number}: no CTest test ${CMAKE_MATCH_1}")
      endif()
    elseif(NOT name IN_LIST gtests)
      problem("index: test ${number}: no GoogleTest test ${name}")
    endif()
  endforeach()
endforeach()
foreach(number RANGE 1 ${count})
  if(NOT number IN_LIST listed_numbers)
    problem("index: contract test ${number} has no row")
  endif()
endforeach()

# Every test named for a conformance test is in its row.
foreach(test IN LISTS gtests)
  set(number "")
  if(test MATCHES "^Conformance0*([0-9]+)")
    set(number "${CMAKE_MATCH_1}")
  elseif(test MATCHES "\\.Conformance0*([0-9]+)")
    set(number "${CMAKE_MATCH_1}")
  endif()
  if(NOT number STREQUAL "")
    if(NOT DEFINED row_names_${number} OR
       NOT test IN_LIST row_names_${number})
      problem("index: ${test} is not named in the row of test ${number}")
    endif()
  endif()
endforeach()

if(problems)
  list(REMOVE_DUPLICATES problems)
  list(JOIN problems "\n  " text)
  message(FATAL_ERROR "tests/conformance.md does not match:\n  ${text}")
endif()
list(LENGTH gtests gtest_count)
message(STATUS "conformance index: ${count} tests of section 15, each "
  "automated (${gtest_count} GoogleTest tests looked at)")
