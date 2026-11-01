# The credentials which all tests share, see setup/credentials.sh
set(CREDENTIALS_DIR ${CMAKE_BINARY_DIR}/tests/credentials)

# add_bats_test(<script> [NAME <name>] [FILTER <regex>]
#               [ENVIRONMENT <variable=value>...] [PROPERTIES <name> <value>...])
#
# Add the BATS file <script> of the current directory as the test
# end-to-end::<subdir>::<stem> for the file tests/end-to-end/<subdir>/<stem>.bats,
# with :: for each / in <subdir>, or as the test NAME. FILTER runs only the
# tests of the file whose names match the regular expression (bats --filter).
# ENVIRONMENT adds variables to the environment of the test, and PROPERTIES
# sets more properties of the test. add_bats_test(<name> <script>) gives the
# name explicitly.
function(add_bats_test)
  cmake_parse_arguments(PARSE_ARGV 0 BATS "" "NAME;FILTER" "ENVIRONMENT;PROPERTIES")
  list(LENGTH BATS_UNPARSED_ARGUMENTS count)
  if(count EQUAL 2)
    list(GET BATS_UNPARSED_ARGUMENTS 0 BATS_NAME)
    list(GET BATS_UNPARSED_ARGUMENTS 1 script)
  else()
    list(GET BATS_UNPARSED_ARGUMENTS 0 script)
  endif()

  if(NOT BATS_NAME)
    file(RELATIVE_PATH subdir ${PROJECT_SOURCE_DIR}/tests/end-to-end ${CMAKE_CURRENT_SOURCE_DIR})
    string(REPLACE "/" "::" subdir "${subdir}")
    get_filename_component(stem ${script} NAME_WE)
    set(BATS_NAME "end-to-end::${subdir}::${stem}")
  endif()

  set(filter)
  if(BATS_FILTER)
    set(filter --filter ${BATS_FILTER})
  endif()

  set(environment
    "BATS_LIB_PATH=${PROJECT_SOURCE_DIR}/vendor/bats/lib/bats"
    "BINARY_DIR=${CMAKE_BINARY_DIR}"
    "CREDENTIALS=${CREDENTIALS_DIR}"
    ${BATS_ENVIRONMENT}
  )

  add_test(NAME ${BATS_NAME}
    COMMAND ${PROJECT_SOURCE_DIR}/vendor/bats/bin/bats --setup-suite-file ${PROJECT_SOURCE_DIR}/tests/end-to-end/setup_suite.bash
            ${filter} ${CMAKE_CURRENT_SOURCE_DIR}/${script})

  set_tests_properties(${BATS_NAME} PROPERTIES
    ENVIRONMENT "${environment}"
    FIXTURES_REQUIRED credentials
    ${BATS_PROPERTIES})
endfunction()

add_subdirectory(setup)

add_subdirectory(apps)
add_subdirectory(authz)
add_subdirectory(cache)
add_subdirectory(cks)
add_subdirectory(cluster)
add_subdirectory(diglib)
add_subdirectory(example)
add_subdirectory(fio)
add_subdirectory(fuse)
add_subdirectory(http)
add_subdirectory(tpc)
add_subdirectory(mirage)
add_subdirectory(monitoring)
add_subdirectory(posix)
add_subdirectory(redirect)
add_subdirectory(sec)
add_subdirectory(xrdclhttp)
add_subdirectory(xrdcl)
add_subdirectory(xrdcp)
add_subdirectory(xrdfs)
