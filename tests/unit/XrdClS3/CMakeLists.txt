# XrdClS3Obj only exists when libcurl was found
if(NOT TARGET XrdClS3Obj)
  return()
endif()

add_unit_tests(
  SOURCES
    XrdClS3DownloadHandler.cc
    XrdClS3Factory.cc
    XrdClS3File.cc
  LIBRARIES
    XrdClS3Obj
)

# The factory reads its options only once in a process, thus the tests of the
# options have their own executable
add_unit_tests(config
  SOURCES
    XrdClS3FactoryConfig.cc
  LIBRARIES
    XrdClS3Obj
)
