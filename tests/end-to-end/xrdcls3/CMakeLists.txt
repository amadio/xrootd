# The tests use the S3 client plugin, which only exists when libcurl was
# found, and an origin with XrdHttp
if(NOT BUILD_HTTP OR NOT TARGET XrdClS3Obj)
  return()
endif()

add_bats_tests(cache.bats)
add_bats_tests(proxy.bats)
