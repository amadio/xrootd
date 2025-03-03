if(NOT BUILD_TPC)
  return()
endif()

set(XrdHttpTPC XrdHttpTPC-${PLUGIN_VERSION})

add_library(${XrdHttpTPC} MODULE
  XrdTpcConfigure.cc
  XrdTpcMultistream.cc
  XrdTpcCurlMulti.cc     XrdTpcCurlMulti.hh
  XrdTpcState.cc         XrdTpcState.hh
  XrdTpcStream.cc        XrdTpcStream.hh
  XrdTpcTPC.cc           XrdTpcTPC.hh
  XrdTpcPMarkManager.cc  XrdTpcPMarkManager.hh
  XrdTpcUtils.cc         XrdTpcUtils.hh)

target_link_libraries(${XrdHttpTPC}
  PRIVATE
    XrdServer
    XrdUtils
    XrdHttpUtils
    CURL::libcurl
    ${CMAKE_THREAD_LIBS_INIT}
    ${CMAKE_DL_LIBS}
)

if (NOT APPLE)
  target_link_options(${XrdHttpTPC} PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/export-lib-symbols")
endif()

install(TARGETS ${XrdHttpTPC} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
