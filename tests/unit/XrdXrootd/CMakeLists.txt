# XrdXrootd unit tests. The tested classes are compiled into the XrdServer
# shared library, so the tests are only built when XrdServer is being built
# (i.e. not in client-only configurations).
if(NOT TARGET XrdServer)
  return()
endif()

add_unit_tests(SOURCES XrdXrootdFileStats.cc XrdXrootdRedirHelper.cc LIBRARIES XrdServer XrdUtils)

# The monitor statics are global to the process and Init() runs only once, so
# these tests have their own executable. The test file provides main().
add_unit_tests(monfile
  SOURCES XrdXrootdMonFile.cc
  LIBRARIES XrdServer XrdUtils
  NO_MAIN
)

# The transfer map tests need the "xfr" option and the largest buffers, which
# are global to the process, so they have their own executable too.
add_unit_tests(monxfr
  SOURCES XrdXrootdMonXfr.cc
  LIBRARIES XrdServer XrdUtils
  NO_MAIN
)

# The tests of the f-stream error records need the "err" option, which is
# global to the process, so they have their own executable too.
add_executable(xrdxrootd-monfile-err-unit-tests XrdXrootdMonFileErr.cc)

target_link_libraries(xrdxrootd-monfile-err-unit-tests
  XrdServer
  XrdUtils
  GTest::gtest
)

gtest_discover_tests(xrdxrootd-monfile-err-unit-tests TEST_PREFIX unit::XrdXrootd::
  PROPERTIES DISCOVERY_TIMEOUT 10)

# The same tests without the "lfn" option
add_executable(xrdxrootd-monfile-err-nolfn-unit-tests XrdXrootdMonFileErr.cc)

target_compile_definitions(xrdxrootd-monfile-err-nolfn-unit-tests
  PRIVATE MONFILEERR_NO_LFN)

target_link_libraries(xrdxrootd-monfile-err-nolfn-unit-tests
  XrdServer
  XrdUtils
  GTest::gtest
)

gtest_discover_tests(xrdxrootd-monfile-err-nolfn-unit-tests
  TEST_PREFIX unit::XrdXrootd::NoLfn::
  PROPERTIES DISCOVERY_TIMEOUT 10)

