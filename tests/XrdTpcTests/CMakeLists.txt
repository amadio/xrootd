add_executable(xrdtpc-unit-tests XrdTpcTests.cc)

add_library(XrdTpcUtils ${CMAKE_SOURCE_DIR}/src/XrdTpc/XrdTpcUtils.cc)

target_link_libraries(xrdtpc-unit-tests XrdTpcUtils GTest::gtest GTest::gtest_main)

gtest_discover_tests(xrdtpc-unit-tests
  PROPERTIES DISCOVERY_TIMEOUT 10)
