if(NOT BUILD_HTTP)
    return()
endif()

add_executable(xrdhttp-unit-tests XrdHttpTests.cc
        ${PROJECT_SOURCE_DIR}/src/XrdHttpCors/XrdHttpCorsHandler.cc)

target_link_libraries(xrdhttp-unit-tests XrdHttpUtils XrdUtils GTest::gtest GTest::gtest_main)

gtest_discover_tests(xrdhttp-unit-tests
  PROPERTIES DISCOVERY_TIMEOUT 10)
