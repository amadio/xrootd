

#-------------------------------------------------------------------------------
# Modules
#-------------------------------------------------------------------------------
set( LIB_XRD_N2NO2P     XrdN2No2p-${PLUGIN_VERSION} )
set( LIB_XRD_CMSREDIRL  XrdCmsRedirectLocal-${PLUGIN_VERSION} )
set( LIB_XRD_GPFS       XrdOssSIgpfsT-${PLUGIN_VERSION} )
set( LIB_XRD_GPI        XrdOfsPrepGPI-${PLUGIN_VERSION} )
set( LIB_XRD_ZCRC32     XrdCksCalczcrc32-${PLUGIN_VERSION} )
set( LIB_XRD_THROTTLE   XrdThrottle-${PLUGIN_VERSION} )

#-------------------------------------------------------------------------------
# Shared library version
#-------------------------------------------------------------------------------

#-------------------------------------------------------------------------------
# N2No2p plugin library
#-------------------------------------------------------------------------------
add_library(
  ${LIB_XRD_N2NO2P}
  MODULE
  XrdOuc/XrdOucN2No2p.cc )

target_link_libraries(
  ${LIB_XRD_N2NO2P}
  PRIVATE
  XrdUtils )

#-------------------------------------------------------------------------------
# GPFS stat() plugin library
#-------------------------------------------------------------------------------
add_library(
  ${LIB_XRD_GPFS}
  MODULE
  XrdOss/XrdOssSIgpfsT.cc )

target_link_libraries(
  ${LIB_XRD_GPFS}
  PRIVATE
  XrdUtils )

#-------------------------------------------------------------------------------
# Ofs Generic Prepare plugin library
#-------------------------------------------------------------------------------
add_library(
  ${LIB_XRD_GPI}
  MODULE
  XrdOfs/XrdOfsPrepGPI.cc )

target_link_libraries(
  ${LIB_XRD_GPI}
  PRIVATE
  XrdUtils )

#-------------------------------------------------------------------------------
# libz compatible CRC32 plugin
#-------------------------------------------------------------------------------

add_library(
  ${LIB_XRD_ZCRC32}
  MODULE
  XrdCks/XrdCksCalczcrc32.cc )

target_link_libraries(
  ${LIB_XRD_ZCRC32}
  PRIVATE
  XrdUtils
  ZLIB::ZLIB)

#-------------------------------------------------------------------------------
# The XrdThrottle lib
#-------------------------------------------------------------------------------
add_library(
  ${LIB_XRD_THROTTLE}
  MODULE
  XrdOfs/XrdOfsFS.cc
  XrdThrottle/XrdThrottle.hh           XrdThrottle/XrdThrottleTrace.hh
  XrdThrottle/XrdThrottleFileSystem.cc
  XrdThrottle/XrdThrottleFileSystemConfig.cc
  XrdThrottle/XrdThrottleFile.cc
  XrdThrottle/XrdThrottleManager.cc    XrdThrottle/XrdThrottleManager.hh
)

target_link_libraries(
  ${LIB_XRD_THROTTLE}
  PRIVATE
  XrdServer
  XrdUtils )

#-------------------------------------------------------------------------------
# The XrdCmsRedirLocal module
#-------------------------------------------------------------------------------
add_library(
  ${LIB_XRD_CMSREDIRL}
  MODULE
  XrdCms/XrdCmsRedirLocal.cc XrdCms/XrdCmsRedirLocal.hh )

target_link_libraries(
  ${LIB_XRD_CMSREDIRL}
  PRIVATE
  XrdServer
  XrdUtils )

#-------------------------------------------------------------------------------
# Install
#-------------------------------------------------------------------------------
install(
  TARGETS ${LIB_XRD_GPFS} ${LIB_XRD_ZCRC32} ${LIB_XRD_THROTTLE} ${LIB_XRD_N2NO2P} ${LIB_XRD_CMSREDIRL} ${LIB_XRD_GPI}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} )
