add_unit_tests(
  SOURCES
    XrdClEnv.cc
    XrdClLocalFileHandler.cc
    XrdClPlugInManager.cc
    XrdClProtocolResp.cc
    XrdClTPFallBackCopyJob.cc
    XrdClURL.cc
    XrdClUtils.cc
    XrdClXRootDResponses.cc
  LIBRARIES
    XrdCl
    XrdTestUtils
)

# The tests load the recorder plug-in from their config files
add_dependencies(xrdcl-unit-tests XrdClRecorder-${PLUGIN_VERSION})
