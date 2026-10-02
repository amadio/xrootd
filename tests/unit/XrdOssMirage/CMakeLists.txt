# The tested code links XrdServer, which a client-only build does not have
if(NOT TARGET XrdServer)
  return()
endif()

add_unit_tests(
  SOURCES
    XrdOssMirage.cc
    XrdOssMirageFile.cc
    XrdOssMirageXAttr.cc
    XrdOssMirageXAttrWrapperTests.cc
    XrdOssMirageWrapperTests.cc
    XrdOssMirageWrapperFileTests.cc
    XrdOssMirageWrapperDirTests.cc
    ${PROJECT_SOURCE_DIR}/src/XrdOssMirage/XrdOssMirage.cc
    ${PROJECT_SOURCE_DIR}/src/XrdOssMirage/XrdOssMirageDir.cc
    ${PROJECT_SOURCE_DIR}/src/XrdOssMirage/XrdOssMirageFile.cc
    ${PROJECT_SOURCE_DIR}/src/XrdOssMirage/XrdOssMirageXAttr.cc
    ${PROJECT_SOURCE_DIR}/src/XrdOssMirage/XrdOssMirageXAttrWrapper.cc
    ${PROJECT_SOURCE_DIR}/src/XrdOssMirage/XrdOssMirageWrapper.cc
    ${PROJECT_SOURCE_DIR}/src/XrdOssMirage/XrdOssMirageWrapperBase.cc
    ${PROJECT_SOURCE_DIR}/src/XrdOssMirage/XrdOssMirageWrapperFile.cc
    ${PROJECT_SOURCE_DIR}/src/XrdOssMirage/XrdOssMirageWrapperDir.cc
  LIBRARIES
    XrdServer
    XrdUtils
)
