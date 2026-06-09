find_program(BATS_COMMAND bats REQUIRED
  HINTS "${PROJECT_SOURCE_DIR}/vendor/bats/bin"
)

find_program(SHELLCHECK shellcheck)

if(EXISTS "${SHELLCHECK}")
  set(ENABLE_SHELLCHECK TRUE)
else()
  set(ENABLE_SHELLCHECK FALSE)
endif()

function(add_integration_tests)
  cmake_parse_arguments(PARSE_ARGV 0 ARG "" "NAME;FILES" "")
  foreach(TEST_FILE ${ARG_FILES})
    get_filename_component(FILE "${TEST_FILE}" ABSOLUTE)
    get_filename_component(NAME "${TEST_FILE}" NAME_WE)

    add_test(
      NAME
        "integration::${ARG_NAME}::${NAME}"
      COMMAND
        ${CMAKE_COMMAND} -E env
          "SOURCE_DIR=${PROJECT_SOURCE_DIR}"
          "BINARY_DIR=${PROJECT_BINARY_DIR}"
        ${BATS_COMMAND} ${FILE}
    )

    if(ENABLE_SHELLCHECK)
      add_test(
        NAME
          "shellcheck::${ARG_NAME}::${NAME}"
        COMMAND
          ${SHELLCHECK} ${FILE}
      )
    endif()
  endforeach()
endfunction()

add_subdirectory(auth)
add_subdirectory(cli)
