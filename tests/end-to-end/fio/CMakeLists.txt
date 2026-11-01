# The tests of the fio engine (src/XrdClFio). They run only when the engine
# was built, and with the fio binary of the version of its build: the engine
# works only with that fio.
if(NOT TARGET fio-xrootd)
  return()
endif()

get_target_property(FIO_TEST_EXECUTABLE fio-xrootd FIO_EXECUTABLE)
if(NOT FIO_TEST_EXECUTABLE)
  return()
endif()

foreach(script cache.bats cluster.bats errors.bats files.bats io.bats
               options.bats pgio.bats readv.bats stats.bats)
  add_bats_test(${script} ENVIRONMENT
    "FIO=${FIO_TEST_EXECUTABLE}" "FIO_ENGINE=$<TARGET_FILE:fio-xrootd>")
endforeach()
