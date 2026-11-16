scada_add_fuzz_target(tag_set_json_fuzz
  SOURCES tag_set_json_fuzz.cpp
  DEPENDS scada::tag)

scada_add_fuzz_target(region_name_fuzz
  SOURCES region_name_fuzz.cpp
  DEPENDS scada::tag)
# region_file.h is private to the library.
target_include_directories(region_name_fuzz
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

scada_add_fuzz_target(header_region_fuzz
  SOURCES header_region_fuzz.cpp
  DEPENDS scada::tag)
# header_view.h is private to the library.
target_include_directories(header_region_fuzz
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

scada_add_fuzz_target(utf8_fuzz
  SOURCES utf8_fuzz.cpp
  DEPENDS scada::tag)
# utf8.h is private to the library.
target_include_directories(utf8_fuzz
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

scada_add_fuzz_target(queue_record_fuzz
  SOURCES queue_record_fuzz.cpp
  DEPENDS scada::tag)
# queue_region.h is private to the library.
target_include_directories(queue_record_fuzz
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

scada_add_fuzz_target(retain_file_fuzz
  SOURCES retain_file_fuzz.cpp
  DEPENDS scada::tag)
# retain_file.h is private to the library.
target_include_directories(retain_file_fuzz
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
