# The tested code links XrdServer, which a client-only build does not have
if(NOT TARGET XrdServer)
  return()
endif()

add_executable(xrdofs-unit-tests XrdOfsStats.cc)

target_link_libraries(xrdofs-unit-tests
  XrdServer
  XrdUtils
  GTest::gtest
  GTest::gtest_main
)

gtest_discover_tests(xrdofs-unit-tests TEST_PREFIX unit::XrdOfs::
  PROPERTIES DISCOVERY_TIMEOUT 10)
