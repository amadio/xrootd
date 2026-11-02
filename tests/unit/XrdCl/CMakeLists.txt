add_unit_tests(
  SOURCES
    XrdClCopyBatchTest.cc
    XrdClCopyChecksumTest.cc
    XrdClEnv.cc
    XrdClLocalFileHandler.cc
    XrdClPlugInManager.cc
    XrdClProtocolResp.cc
    XrdClReadRangesTest.cc
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

# The plug-in manager reads its configuration once, when XrdCl loads. The
# configs in no-plugins settle the URLs of the tests, thus a config of the
# host, for example /etc/xrootd/client.plugins.d/http.conf, cannot replace
# the factories of the tests. The PlugInManager tests set their own config
# directory, which a value from the environment blocks.
add_unit_tests(dirlist
  SOURCES
    XrdClDirListTest.cc
  LIBRARIES
    XrdCl
  PROPERTIES
    ENVIRONMENT "XRD_PLUGINCONFDIR=${CMAKE_CURRENT_SOURCE_DIR}/no-plugins"
)
