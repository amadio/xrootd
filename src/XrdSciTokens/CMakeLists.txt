unset(BUILD_SCITOKENS CACHE)

if(NOT ENABLE_SCITOKENS)
  return()
endif()

if(FORCE_ENABLED)
  find_package(Macaroons REQUIRED)
  find_package(SciTokensCpp REQUIRED)
else()
  find_package(Macaroons)
  find_package(SciTokensCpp)
endif()

if(SciTokensCpp_FOUND)
  set(BUILD_SCITOKENS TRUE CACHE INTERNAL "")
else()
  return()
endif()

set(XrdAccSciTokens XrdAccSciTokens-${PLUGIN_VERSION})

add_library(${XrdAccSciTokens} MODULE
  XrdSciTokensAccess.cc XrdSciTokensHelper.hh
  XrdSciTokensMon.cc    XrdSciTokensMon.hh
                        XrdSciTokensRedir.hh
                        XrdSciTokensHelper.hh

  ../XrdMacaroons/XrdMacaroonsUtils.cc
  ../XrdMacaroons/XrdMacaroonsUtils.hh
  ../XrdMacaroons/XrdMacaroonsGenerate.cc
  ../XrdMacaroons/XrdMacaroonsGenerate.hh
  ../XrdMacaroons/XrdMacaroonsConfigure.cc
)

target_link_libraries(${XrdAccSciTokens}
  PRIVATE
    XrdUtils
    XrdServer
    ${SCITOKENS_CPP_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${CMAKE_DL_LIBS}
)

target_include_directories(${XrdAccSciTokens}
  PRIVATE
    vendor/inih
    vendor/picojson
    ${SCITOKENS_CPP_INCLUDE_DIR}
)

if(MACAROONS_FOUND)
  target_compile_definitions(${XrdAccSciTokens} PRIVATE HAVE_MACAROONS)
  target_include_directories(${XrdAccSciTokens} PRIVATE ${MACAROONS_INCLUDES})
  target_link_libraries(${XrdAccSciTokens} PRIVATE uuid::uuid OpenSSL::Crypto ${MACAROONS_LIB})
endif()

if(HAVE_SCITOKEN_CONFIG_SET_STR)
  target_compile_definitions(${XrdAccSciTokens}
    PRIVATE
      HAVE_SCITOKEN_CONFIG_SET_STR
  )
endif()

install(
  TARGETS
    ${XrdAccSciTokens}
  LIBRARY
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
