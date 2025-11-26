#.rst:
# Findpicojson
# -----------
#
# Find picojson, header-only JSON parser serializer in C++
#
# Imported Targets
# ^^^^^^^^^^^^^^^^
#
# This module defines :prop_tgt:`IMPORTED` target:
#
# ``picojson::picojson``
#   The picojson interface library, if found.
#
# Result Variables
# ^^^^^^^^^^^^^^^^
#
# This module will set the following variables in your project:
#
# ``PICOJSON_FOUND``
#   True if picojson has been found.
# ``PICOJSON_INCLUDE_DIR``
#   Where to find picojson.h.

include(FindPackageHandleStandardArgs)

if(NOT PICOJSON_INCLUDE_DIR)
  set(CMAKE_FIND_FRAMEWORK LAST)
  find_path(PICOJSON_INCLUDE_DIR picojson.h)
endif()

find_package_handle_standard_args(picojson DEFAULT_MSG PICOJSON_INCLUDE_DIR)

if(PICOJSON_FOUND)
  if(NOT TARGET picojson::picojson)
    add_library(picojson::picojson INTERFACE IMPORTED)
    target_include_directories(picojson::picojson SYSTEM INTERFACE "${PICOJSON_INCLUDE_DIR}")
  endif()
endif()

mark_as_advanced(PICOJSON_INCLUDE_DIR)

if(NOT "${picojson_FIND_QUIET}")
  message(DEBUG "PICOJSON_FOUND        = ${PICOJSON_FOUND}")
  message(DEBUG "PICOJSON_INCLUDE_DIR  = ${PICOJSON_INCLUDE_DIR}")
endif()
