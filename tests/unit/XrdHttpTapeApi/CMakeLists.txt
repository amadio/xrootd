if(NOT TARGET XrdHttpTapeApiStore)
  return()
endif()

add_unit_tests(SOURCES XrdHttpTapeApiStore.cc LIBRARIES XrdHttpTapeApiStore)

target_include_directories(xrdhttptapeapi-unit-tests
  BEFORE PRIVATE
    "${PROJECT_SOURCE_DIR}/vendor/googletest/googletest/include"
    "${PROJECT_SOURCE_DIR}/src/XrdHttpTapeApi")
