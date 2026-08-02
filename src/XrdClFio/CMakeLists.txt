#-------------------------------------------------------------------------------
# fio external I/O engine for XRootD, built on the asynchronous XrdCl::File API.
#
# Produces fio-xrootd.so, loaded by fio as:
#   fio --ioengine=external:<libdir>/fio-xrootd.so \
#       --filename=root://host:1094//path/to/file ...
#
# Building requires a *configured* fio source tree (so that config-host.h and
# the fio headers exist): set -DFIO_SOURCE_DIR=/path/to/fio and run its
# ./configure once. The engine is pinned to that fio's FIO_IOOPS_VERSION and
# must be rebuilt if the fio binary it runs with is upgraded.
#-------------------------------------------------------------------------------

set(FIO_SOURCE_DIR "" CACHE PATH
    "Path to a configured fio source tree (must contain config-host.h)")

if(NOT FIO_SOURCE_DIR OR NOT EXISTS "${FIO_SOURCE_DIR}/config-host.h")
  message(FATAL_ERROR
    "ENABLE_FIO_ENGINE is ON but FIO_SOURCE_DIR does not point to a configured "
    "fio checkout. Set -DFIO_SOURCE_DIR=/path/to/fio and run './configure' in "
    "that tree first so that config-host.h exists.")
endif()

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
target_compile_options(fio-xrootd
  PRIVATE
    -D_GNU_SOURCE
    -include ${FIO_SOURCE_DIR}/config-host.h
    -Wno-invalid-offsetof
    -Wno-missing-field-initializers
)

target_link_libraries(fio-xrootd PRIVATE XrdCl)

# The engine is a plugin loaded by fio, not part of the XRootD runtime, so keep
# it out of the main library directory.
install(TARGETS fio-xrootd
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/xrootd/fio)
