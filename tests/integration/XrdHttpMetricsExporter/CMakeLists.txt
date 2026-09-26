# The push helper of the metrics exporter needs libcurl, as the push support
# of the plugin does. The test pushes to an HTTP receiver on the loopback.
find_package(CURL)
if(NOT CURL_FOUND)
  return()
endif()

add_executable(xrdhttpmetricsexporter-integration-tests
  XrdHttpMetricsExporterPush.cc)

target_compile_definitions(xrdhttpmetricsexporter-integration-tests
  PRIVATE XRDMETRICS_HAVE_CURL)

target_link_libraries(xrdhttpmetricsexporter-integration-tests
  XrdUtils CURL::libcurl GTest::gtest GTest::gtest_main)

gtest_discover_tests(xrdhttpmetricsexporter-integration-tests
  TEST_PREFIX integration::XrdHttpMetricsExporter::
  PROPERTIES DISCOVERY_TIMEOUT 10)
