add_executable(xrdmoncollect-unit-tests
  XrdMonCollectTests.cc
  XrdMonDiskCacheTests.cc
  XrdMonOtlpTests.cc
  XrdMonShovelFrameTests.cc XrdMonFilterTests.cc
  ${CMAKE_SOURCE_DIR}/src/XrdApps/XrdMonCollect/XrdMonDiskCache.cc
  ${CMAKE_SOURCE_DIR}/src/XrdApps/XrdMonCollect/XrdMonOtlp.cc)

target_link_libraries(xrdmoncollect-unit-tests
  XrdAppUtils
  XrdUtils
  GTest::gtest
  GTest::gtest_main)

gtest_discover_tests(xrdmoncollect-unit-tests
  PROPERTIES DISCOVERY_TIMEOUT 10)

# Configuration-file behaviour of the xrdmoncollect executable (built only when
# not XRDCL_ONLY). A small shell driver runs the binary against temporary INI
# files, checking deterministic, non-binding paths (value applied, CLI override,
# malformed/missing file errors).
if(TARGET xrdmoncollect)
  add_test(NAME XrdMonCollect::ConfigFile
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/config_test.sh
            $<TARGET_FILE:xrdmoncollect>)

  # Systemd socket activation (LISTEN_FDS): the harness plays PID 1, holds the
  # UDP socket across a collector restart, and asserts that packets sent while
  # no collector was running come out fully correlated afterwards. Only
  # meaningful when built with libsystemd (sd_listen_fds).
  if(SYSTEMD_FOUND)
    add_test(NAME XrdMonCollect::SocketActivation
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/socket_activation_test.sh
              $<TARGET_FILE:xrdmoncollect>)
    set_tests_properties(XrdMonCollect::SocketActivation
      PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
  endif()
endif()

# The Grafana dashboards and OpenSearch objects shipped next to the collector
# are data, not code: a panel querying a metric that has been renamed, or a new
# metric that reaches no dashboard at all, compiles and packages perfectly and
# only shows up as an empty panel in somebody's Grafana. This checks both
# directions against the collector sources. It needs no build artefact, so it
# runs even where the daemon itself is not built.
find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_Interpreter_FOUND)
  add_test(NAME XrdMonCollect::Dashboards
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/dashboards_test.py ${CMAKE_SOURCE_DIR})
  set_tests_properties(XrdMonCollect::Dashboards PROPERTIES TIMEOUT 60)
endif()
