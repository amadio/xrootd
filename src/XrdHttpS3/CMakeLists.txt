if(NOT BUILD_HTTP)
  return()
endif()

set(XrdHttpS3 XrdHttpS3-${PLUGIN_VERSION})

# The listing does not access the file system, thus the unit tests link it
add_library(XrdHttpS3List STATIC XrdHttpS3List.cc XrdHttpS3List.hh)
set_target_properties(XrdHttpS3List PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
target_link_libraries(XrdHttpS3List PUBLIC XrdUtils)

add_library(${XrdHttpS3} MODULE XrdHttpS3Handler.cc)
target_link_libraries(${XrdHttpS3} PRIVATE XrdHttpUtils XrdHttpS3List)
