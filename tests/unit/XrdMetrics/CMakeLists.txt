add_executable(xrdmetrics-unit-tests
  XrdMetricsAggregate.cc
  XrdMetricsConfig.cc
  XrdMetricsRegistry.cc)

target_link_libraries(xrdmetrics-unit-tests XrdUtils GTest::gtest GTest::gtest_main)

gtest_discover_tests(xrdmetrics-unit-tests TEST_PREFIX unit::XrdMetrics::
  PROPERTIES DISCOVERY_TIMEOUT 10)
