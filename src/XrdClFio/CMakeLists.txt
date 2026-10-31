#-------------------------------------------------------------------------------
# fio external I/O engine for XRootD, built on the asynchronous XrdCl::File API.
#
# Produces fio-xrootd.so, loaded by fio as:
#   fio --ioengine=external:<libdir>/fio-xrootd.so \
#       --filename='root\://host\:1094//path/to/file' ...
#
# fio ships no development headers (no distro fio-devel package), and its engine
# ABI is version-locked (FIO_IOOPS_VERSION), so the engine must be compiled
# against fio's source headers at the *same version* as the fio binary it will
# run with. This is handled automatically:
#
#   * If FIO_SOURCE_DIR is set, that (configured) fio checkout is used.
#   * Otherwise the fio binary is located, its version queried, and the matching
#     fio source is cloned and configured on the fly.
#
# The engine is best-effort: if fio (or git, or the network) is unavailable, or
# under a sanitizer build, it is skipped with a message rather than failing the
# whole configure. With FORCE_ENABLED, the configure fails instead.
#-------------------------------------------------------------------------------

set(FIO_SOURCE_DIR "" CACHE PATH
    "Configured fio source tree (with config-host.h); auto-fetched to match the installed fio if empty")

# Skip the engine with a message. With FORCE_ENABLED, stop the configuration
# instead, as for the other optional components. The return() of a macro
# leaves the directory.
macro(fio_engine_skip reason)
  if(FORCE_ENABLED)
    message(FATAL_ERROR "XrdClFio: ${reason}")
  endif()
  message(STATUS "XrdClFio: ${reason}, skipping fio engine")
  return()
endmacro()

# The engine is dlopen'd by the fio binary, which is not sanitizer-instrumented,
# so an instrumented engine cannot be loaded. Skip under sanitizer builds.
if(ENABLE_ASAN OR ENABLE_TSAN OR CMAKE_CXX_FLAGS MATCHES "-fsanitize")
  fio_engine_skip("sanitizer build detected")
endif()

# fio external engines and the integration test are Linux-only here.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  fio_engine_skip("non-Linux platform")
endif()

find_program(FIO_EXECUTABLE fio)

if(NOT FIO_SOURCE_DIR)
  if(NOT FIO_EXECUTABLE)
    fio_engine_skip("fio not found and FIO_SOURCE_DIR unset")
  endif()

  # Query the installed fio version and fetch the matching source, since the
  # engine's FIO_IOOPS_VERSION must equal the running fio's.
  execute_process(COMMAND ${FIO_EXECUTABLE} --version
    OUTPUT_VARIABLE FIO_VERSION_STRING
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

  if(NOT FIO_VERSION_STRING MATCHES "fio-([0-9]+\\.[0-9]+)")
    fio_engine_skip("could not parse fio version from '${FIO_VERSION_STRING}'")
  endif()

  set(FIO_GIT_TAG "fio-${CMAKE_MATCH_1}")
  set(FIO_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fio-src")

  if(NOT EXISTS "${FIO_SOURCE_DIR}/fio.h")
    find_program(GIT_EXECUTABLE git)
    if(NOT GIT_EXECUTABLE)
      fio_engine_skip("git not found, cannot fetch fio source")
    endif()
    message(STATUS "XrdClFio: fetching ${FIO_GIT_TAG} source to match the installed fio")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} clone --depth 1 --branch ${FIO_GIT_TAG}
              https://github.com/axboe/fio.git "${FIO_SOURCE_DIR}"
      RESULT_VARIABLE FIO_CLONE_RC ERROR_VARIABLE FIO_CLONE_ERR OUTPUT_QUIET)
    if(NOT FIO_CLONE_RC EQUAL 0)
      string(STRIP "${FIO_CLONE_ERR}" FIO_CLONE_ERR)
      fio_engine_skip("failed to fetch fio source (${FIO_CLONE_ERR})")
    endif()
  endif()
endif()

# fio requires a generated config-host.h; run its configure once if missing.
if(NOT EXISTS "${FIO_SOURCE_DIR}/config-host.h")
  message(STATUS "XrdClFio: generating fio config-host.h in ${FIO_SOURCE_DIR}")
  execute_process(COMMAND ./configure
    WORKING_DIRECTORY "${FIO_SOURCE_DIR}" OUTPUT_QUIET ERROR_QUIET)
endif()

if(NOT EXISTS "${FIO_SOURCE_DIR}/config-host.h" OR NOT EXISTS "${FIO_SOURCE_DIR}/fio.h")
  fio_engine_skip("fio headers unavailable in '${FIO_SOURCE_DIR}'")
endif()

message(STATUS "XrdClFio: building fio engine against ${FIO_SOURCE_DIR}")

add_library(fio-xrootd MODULE XrdClFioEngine.cc)

set_target_properties(fio-xrootd
  PROPERTIES
    PREFIX ""
    OUTPUT_NAME "fio-xrootd"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(fio-xrootd
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src          # for "XrdCl/XrdCl*.hh"
    ${FIO_SOURCE_DIR}                # for "fio.h", "optgroup.h", ...
    ${FIO_SOURCE_DIR}/engines
)

# -include config-host.h: fio requires its generated config to be force-included.
# -Wno-invalid-offsetof: fio's intrusive list (container_of/offsetof) is used on
#   a type that embeds a C++ handler; the list node is the first member so the
#   offset is 0 and the computation is correct in practice.
# -Wno-missing-field-initializers: the option table uses designated initializers.
# -Wno-pointer-arith: fio's parse.h (td_var) does void* arithmetic, a GNU
#   extension flagged by older compilers (e.g. GCC 8 on AlmaLinux 8); it is in
#   fio's headers, not ours, so silence it for this target.
target_compile_options(fio-xrootd
  PRIVATE
    -D_GNU_SOURCE
    -include ${FIO_SOURCE_DIR}/config-host.h
    -Wno-invalid-offsetof
    -Wno-missing-field-initializers
    -Wno-pointer-arith
)

# XrdPosix maps the XRootD status codes to POSIX errno values.
target_link_libraries(fio-xrootd PRIVATE XrdCl XrdPosix XrdUtils)

# The engine is a plugin loaded by fio, not part of the XRootD runtime, so keep
# it out of the main library directory.
install(TARGETS fio-xrootd
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/xrootd/fio)
