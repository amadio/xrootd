# Benchmarks of the client and the server with fio and the fio engine (see
# tests/end-to-end/fio). Each benchmark is a test of BATS in a file of this
# directory, and the CTest test benchmark::<file>::<benchmark>. A benchmark
# prints its results as measurements of CTest, which CDash shows for each
# submission. The benchmarks run one at a time, as their results depend on
# the load of the host. Run them with: ctest -L benchmark
if(NOT TARGET fio-xrootd)
  message(STATUS "Benchmarks: no fio engine (ENABLE_FIO_ENGINE), skipping the benchmarks")
  return()
endif()

get_target_property(FIO_TEST_EXECUTABLE fio-xrootd FIO_EXECUTABLE)
if(NOT FIO_TEST_EXECUTABLE)
  message(STATUS "Benchmarks: no fio for the fio engine, skipping the benchmarks")
  return()
endif()

# add_benchmarks(<script> <benchmark>...)
#
# Add each benchmark of the BATS file as the test
# benchmark::<stem of the file>::<benchmark> (see add_bats_test), which runs
# only that benchmark, alone.
function(add_benchmarks script)
  get_filename_component(stem ${script} NAME_WE)
  foreach(benchmark ${ARGN})
    add_bats_test(${script} NAME benchmark::${stem}::${benchmark}
      FILTER "^${benchmark}$"
      ENVIRONMENT "FIO=${FIO_TEST_EXECUTABLE}" "FIO_ENGINE=$<TARGET_FILE:fio-xrootd>"
      PROPERTIES LABELS benchmark RUN_SERIAL TRUE TIMEOUT 300)
  endforeach()
endfunction()

add_benchmarks(server.bats
  read-seq-1m read-seq-1m-4jobs read-seq-1m-pgio read-rand-4k
  read-rand-4k-depth1 read-rand-64k-readv read-root-analysis
  read-root-analysis-readv read-small-files write-seq-1m write-seq-1m-pgio
  write-rand-4k rw-rand-64k)

add_benchmarks(cache.bats
  cache-read-seq-1m-cold cache-read-seq-1m-warm cache-read-rand-4k-warm
  cache-root-analysis-64k cache-root-analysis-128k cache-root-analysis-512k
  cache-root-analysis-1m cache-root-analysis-1m-prefetch
  cache-root-analysis-1m-warm)
