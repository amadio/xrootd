# XrdClHttpObj only exists when libcurl was found
if(NOT TARGET XrdClHttpObj)
  return()
endif()

find_package(CURL REQUIRED)

add_unit_tests(
  SOURCES
    ClientConfigTest.cc
    CopyResponseTest.cc
    OptionsCacheTest.cc
    ReadResponseTest.cc
    RedirectTest.cc
    SpaceResponseTest.cc
    WebDavParserTest.cc
    XrdClHttpHeaderBuilderAppendMissing.cc
    XrdClHttpHeaderBuilderEntrySeparation.cc
    XrdClHttpHeaderBuilderForbiddenEntry.cc
    XrdClHttpHeaderBuilderForbiddenHeader.cc
    XrdClHttpHeaderBuilderParsing.cc
    XrdClHttpParseTimeout.cc
    XrdClHttpTape.cc
    XrdClHttpTokenFileParser.cc
    XrdClHttpUtil.cc
  LIBRARIES
    XrdClHttpObj
    XrdCl
    XrdXml
    CURL::libcurl
  INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
)

# The tests of the third-party copy compile XrdClHttpFilesystem.cc
# themselves, thus they link the plug-in without it.
file(GLOB_RECURSE XrdClHttpWithoutFilesystemSources
  ${PROJECT_SOURCE_DIR}/src/XrdClHttp/*.cc
)
list(REMOVE_ITEM XrdClHttpWithoutFilesystemSources
  ${PROJECT_SOURCE_DIR}/src/XrdClHttp/XrdClHttpFilesystem.cc
)

add_library(XrdClHttpWithoutFilesystem OBJECT
  ${XrdClHttpWithoutFilesystemSources}
)

target_link_libraries(XrdClHttpWithoutFilesystem
  PRIVATE
    XrdCl
    XrdUtils
    XrdXml
    CURL::libcurl
    OpenSSL::Crypto
)

add_unit_tests(filesystem
  SOURCES XrdClHttpFilesystem.cc
  LIBRARIES XrdClHttpWithoutFilesystem
)

# Exercise the actual worker with test-only aliases for the curl driver calls.
# Keep this object library separate so the installed plugin never contains the
# failure injection and there are no duplicate production symbols at link time.
add_library(XrdClHttpLifecycle OBJECT
  ${PROJECT_SOURCE_DIR}/src/XrdClHttp/XrdClHttpHeaderBuilder.cc
  ${PROJECT_SOURCE_DIR}/src/XrdClHttp/XrdClHttpOptionsCache.cc
  WorkerLifecycleUtil.cc WorkerLifecycleOps.cc WorkerLifecycleOptions.cc)
target_link_libraries(XrdClHttpLifecycle PRIVATE
  XrdCl XrdUtils XrdXml CURL::libcurl OpenSSL::Crypto Threads::Threads)
target_include_directories(XrdClHttpLifecycle PRIVATE
  "${PROJECT_SOURCE_DIR}/src")

add_unit_tests(worker
  SOURCES XrdClHttpWorker.cc
  LIBRARIES XrdClHttpLifecycle XrdCl CURL::libcurl
  INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/src
  PROPERTIES TIMEOUT 10
)

# Per-file credentials coverage uses a disposable localhost HTTP server.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME XrdClHttp::FileCredentials
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/FileCredentialsTest.py"
      --xrdcp $<TARGET_FILE:xrdcp>
      --plugin $<TARGET_FILE:XrdClHttp-${PLUGIN_VERSION}>)
  set_tests_properties(XrdClHttp::FileCredentials PROPERTIES TIMEOUT 150)

  if(TARGET xrdcp AND TARGET xrdfs)
    add_executable(xrdcl-http-upload-buffer UploadBufferDriver.cc)
    target_link_libraries(xrdcl-http-upload-buffer PRIVATE XrdClHttpObj XrdCl)
    target_include_directories(xrdcl-http-upload-buffer
      PRIVATE "${PROJECT_SOURCE_DIR}/src")
    add_test(NAME XrdClHttp::cli
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cli.py"
        "$<TARGET_FILE_DIR:xrdcp>" "$<TARGET_FILE:XrdClHttp-${PLUGIN_VERSION}>"
        "$<TARGET_FILE:xrdcl-http-upload-buffer>")
    set_tests_properties(XrdClHttp::cli PROPERTIES TIMEOUT 120)
  endif()
endif()
