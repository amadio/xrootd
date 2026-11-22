if(NOT TARGET XrdHttpS3List)
  return()
endif()

add_unit_tests(
  SOURCES XrdHttpS3List.cc
  LIBRARIES XrdHttpS3List
  INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/src/XrdHttpS3
)
