# The provider keeps its protocol class private to its source, so the test
# compiles that source directly and calls its C factory, rather than loading
# the plugin module.

add_unit_tests(
  SOURCES
    XrdSecProtocolunix.cc
    ${PROJECT_SOURCE_DIR}/src/XrdSecunix/XrdSecProtocolunix.cc
  LIBRARIES
    XrdUtils
)
