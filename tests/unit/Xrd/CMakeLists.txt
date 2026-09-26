add_unit_tests(SOURCES XrdScheduler.cc LIBRARIES XrdUtils)

# The stats tests link XrdServer, which a client-only build does not have
if(NOT TARGET XrdServer)
  return()
endif()

add_executable(xrd-stats-unit-tests
  XrdSchedulerStats.cc
  XrdStatsLegacy.cc)

target_link_libraries(xrd-stats-unit-tests
  XrdServer
  XrdUtils
  GTest::gtest
  GTest::gtest_main
)

gtest_discover_tests(xrd-stats-unit-tests TEST_PREFIX unit::Xrd::
  PROPERTIES DISCOVERY_TIMEOUT 10)
