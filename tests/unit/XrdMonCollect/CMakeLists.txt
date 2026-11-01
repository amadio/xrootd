add_executable(xrdmoncollect-unit-tests
  XrdMonBulk.cc
  XrdMonDecode.cc
  XrdMonDiskCache.cc
  XrdMonFilter.cc
  XrdMonMemory.cc
  XrdMonOtlp.cc
  XrdMonShovelFrame.cc
  XrdMonUtil.cc
  ${CMAKE_SOURCE_DIR}/src/XrdApps/XrdMonCollect/XrdMonDiskCache.cc
  ${CMAKE_SOURCE_DIR}/src/XrdApps/XrdMonCollect/XrdMonMemory.cc
  ${CMAKE_SOURCE_DIR}/src/XrdApps/XrdMonCollect/XrdMonOtlp.cc)

target_link_libraries(xrdmoncollect-unit-tests
  XrdAppUtils
  XrdUtils
  GTest::gtest
  GTest::gtest_main)

gtest_discover_tests(xrdmoncollect-unit-tests TEST_PREFIX unit::XrdMonCollect::
  PROPERTIES DISCOVERY_TIMEOUT 10)
