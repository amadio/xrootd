add_executable(xrdmoncollect-unit-tests
  XrdMonCollectTests.cc
  XrdMonBulkTests.cc
  XrdMonDiskCacheTests.cc
  XrdMonOtlpTests.cc
  XrdMonShovelFrameTests.cc XrdMonFilterTests.cc XrdMonMemoryTests.cc
  XrdMonUtilTests.cc
  ${CMAKE_SOURCE_DIR}/src/XrdApps/XrdMonCollect/XrdMonDiskCache.cc
  ${CMAKE_SOURCE_DIR}/src/XrdApps/XrdMonCollect/XrdMonMemory.cc
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

  # Housekeeping (the idle-incarnation reap, and with it the memory cap) has to
  # run on the wall clock rather than on packet arrival. Sends one packet, then
  # stays silent past --server-ttl and the sweep interval. Slow by nature: the
  # sweep is gated at 60s and only a timed wake-up can reach it.
  add_test(NAME XrdMonCollect::IdleReap
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/idle_reap_test.sh
            $<TARGET_FILE:xrdmoncollect>)
  set_tests_properties(XrdMonCollect::IdleReap
    PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 200)

  # A finished session leaves the state on its own. Drives a real login and
  # isDisc over the wire, then waits out --user-ttl and the sweep. Slow for the
  # same reason as IdleReap, and slow in parallel with it.
  add_test(NAME XrdMonCollect::UserTTL
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/user_ttl_test.sh
            $<TARGET_FILE:xrdmoncollect>)
  set_tests_properties(XrdMonCollect::UserTTL
    PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 200)

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
