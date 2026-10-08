# Reuse the native clients, client libraries and dynamically loaded plugins.
# The same payload target is used by setup.py and the enclosing CMake wheel.

set(PYXROOTD_COMMANDS)

if(PYPI_BUILD AND EXISTS ${CMAKE_SOURCE_DIR}/src/XrdApps/XrdToken.cc
   AND NOT TARGET xrdtoken)
  message(FATAL_ERROR "The token client source is present but xrdtoken is disabled")
endif()
set(PYXROOTD_PAYLOAD_TARGETS client)

if(TARGET xrdfs AND TARGET xrdcp)
  set(PYXROOTD_COMMANDS xrdfs xrdcp xrdcopy)
  list(APPEND PYXROOTD_PAYLOAD_TARGETS xrdfs xrdcp)
  if(TARGET xrdtoken)
    list(APPEND PYXROOTD_COMMANDS xrdtoken)
    list(APPEND PYXROOTD_PAYLOAD_TARGETS xrdtoken)
  endif()

  # These directories own the client dependencies and loadable plugins. Avoid
  # server libraries even when packaging a wheel from a full server build.
  foreach(directory XrdApps XrdCks XrdCl XrdClHttp XrdClS3 XrdCrypto XrdPosix
                    XrdSec XrdSecgsi XrdSeckrb5 XrdSecpwd XrdSecsss XrdSecunix
                    XrdSecztn XrdXml)
    get_property(targets DIRECTORY ${CMAKE_SOURCE_DIR}/src/${directory}
                 PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
      get_target_property(type ${target} TYPE)
      if(type STREQUAL "SHARED_LIBRARY" OR type STREQUAL "MODULE_LIBRARY")
        list(APPEND PYXROOTD_PAYLOAD_TARGETS ${target})
      endif()
    endforeach()
  endforeach()
  list(APPEND PYXROOTD_PAYLOAD_TARGETS XrdUtils)
endif()

if(PYPI_BUILD AND (NOT PYXROOTD_COMMANDS OR NOT TARGET XrdClHttp-${PLUGIN_VERSION}))
  message(FATAL_ERROR "PyPI builds require native clients and the HTTP plugin")
endif()

set(PYXROOTD_PAYLOAD_DIR "${CMAKE_CURRENT_BINARY_DIR}/extension"
    CACHE PATH "Staging directory for the Python wheel native client payload")

set(payload_commands
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${PYXROOTD_PAYLOAD_DIR}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PYXROOTD_PAYLOAD_DIR})

if(PYXROOTD_COMMANDS AND TARGET XrdClHttp-${PLUGIN_VERSION})
  file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/http.conf CONTENT
    "url = http://*;https://*;dav://*;davs://*\nlib = $<TARGET_FILE_NAME:XrdClHttp-${PLUGIN_VERSION}>\nenable = true\n")
  list(APPEND payload_commands COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_BINARY_DIR}/http.conf ${PYXROOTD_PAYLOAD_DIR})
endif()

foreach(target IN LISTS PYXROOTD_PAYLOAD_TARGETS)
  if(PYXROOTD_COMMANDS)
    # Relative paths come first in the wheel; the usual build paths still allow
    # these same targets to run in the original CMake build tree. Install paths
    # are retained for regular native installations as well.
    if(APPLE)
      set(relative_rpath "@loader_path")
      set_target_properties(${target} PROPERTIES
        INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR TRUE)
    else()
      set(relative_rpath "\$ORIGIN")
    endif()
    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH ${relative_rpath})
    set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH ${relative_rpath})
  endif()

  get_target_property(type ${target} TYPE)
  if(type STREQUAL "SHARED_LIBRARY")
    # Preserve the SONAME needed by the loader and the unversioned name used
    # by dlopen. Copies avoid symlinks, which wheel ZIPs cannot preserve.
    list(APPEND payload_commands
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${target}>
              ${PYXROOTD_PAYLOAD_DIR}/$<TARGET_SONAME_FILE_NAME:${target}>
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${target}>
              ${PYXROOTD_PAYLOAD_DIR}/$<TARGET_LINKER_FILE_NAME:${target}>)
  else()
    list(APPEND payload_commands
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${target}>
              ${PYXROOTD_PAYLOAD_DIR})
  endif()
endforeach()

add_custom_target(PyXRootDClientPayload
  ${payload_commands}
  DEPENDS ${PYXROOTD_PAYLOAD_TARGETS}
  COMMENT "Staging Python wheel native clients and plugins"
  VERBATIM)

# setup.py needs a space-separated list when substituted into Python source.
string(JOIN " " PYXROOTD_COMMANDS ${PYXROOTD_COMMANDS})
