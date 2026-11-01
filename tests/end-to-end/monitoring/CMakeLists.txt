if(BUILD_HTTP)
  add_bats_test(http.bats)
endif()

# The tests of the monitoring collector, which a client-only build does not
# have
if(TARGET xrdmoncollect)
  # XrdCl programs which make the server report an error on the f-stream:
  # xrdreadv-eof reads past the end of a file, and xrdopen-denied opens a
  # file for write two times, thus the server denies the second open.
  add_executable(xrdreadv-eof readv_eof.cc)
  target_link_libraries(xrdreadv-eof PRIVATE XrdCl)

  add_executable(xrdopen-denied open_denied.cc)
  target_link_libraries(xrdopen-denied PRIVATE XrdCl)

  add_bats_test(collector.bats)
  add_bats_test(shovel.bats)
endif()
