# XrdPfc is a loadable module, so the self-contained CacheMetrics source is
# compiled directly into the test; it only needs the header-only
# registry/instruments and the serializer from XrdUtils.
add_executable(xrdpfc-metrics-unit-tests
  XrdPfcMetrics.cc
  ${CMAKE_SOURCE_DIR}/src/XrdPfc/XrdPfcMetrics.cc)

target_link_libraries(xrdpfc-metrics-unit-tests XrdUtils GTest::gtest GTest::gtest_main)

gtest_discover_tests(xrdpfc-metrics-unit-tests TEST_PREFIX unit::XrdPfc::
  PROPERTIES DISCOVERY_TIMEOUT 10)

# The tested code links XrdServer, which a client-only build does not have
if(NOT TARGET XrdServer)
  return()
endif()

# XrdPfcInfo.cc and XrdPfcDirState.cc are compiled in rather than linked from
# the plugin, as neither needs the Cache singleton.
add_unit_tests(
  SOURCES
    XrdPfcDirState.cc
    XrdPfcInfo.cc
    XrdPfcPathParseTools.cc
    ${PROJECT_SOURCE_DIR}/src/XrdPfc/XrdPfcInfo.cc
    ${PROJECT_SOURCE_DIR}/src/XrdPfc/XrdPfcDirState.cc
  LIBRARIES
    XrdServer
    XrdUtils
)
