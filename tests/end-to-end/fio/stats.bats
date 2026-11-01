#!/usr/bin/env bash

# The statistics of the submissions of the fio engine. fio leaves them to an
# engine with a commit hook: the submission latency (slat) and the histogram
# of the submissions. Each test has its own server.

load fio.bash

setup_file() {
    require_fio
}

setup() {
    start_server
    cd "$BATS_TEST_TMPDIR"
    make_random_file "$SERVER_DATA/data.dat" 4194304
}

teardown() {
    stop_servers
}

@test "the submission latency has a sample for each read" {
    fio_json --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randread --bs=4k --number_ios=64 --iodepth=8
    run -0 fio_result '.jobs[0].read.slat_ns.N'
    assert_output 64
}

@test "the submission latency has a sample for each read of a vector read" {
    fio_json --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randread --bs=4k --number_ios=64 --iodepth=8 --iodepth_batch=8 \
        --xrd_readv=1
    run -0 fio_result '.jobs[0].read.slat_ns.N'
    assert_output 64
}

@test "the submission latency has a sample for each write" {
    fio_json --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randwrite --bs=4k --number_ios=64 --iodepth=8
    run -0 fio_result '.jobs[0].write.slat_ns.N'
    assert_output 64
}

@test "the histogram of the submissions counts single reads" {
    fio_json --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randread --bs=4k --number_ios=64
    run -0 fio_result '.jobs[0].iodepth_submit."4"'
    assert_output 100.000000
}

@test "the histogram of the submissions counts vector reads of a batch" {
    fio_json --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randread --bs=4k --number_ios=16 --iodepth=16 --iodepth_batch=16 \
        --xrd_readv=1
    run -0 fio_result '.jobs[0].iodepth_submit."16"'
    assert_output 100.000000
}

@test "the completion latency has a sample for each read" {
    fio_json --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randread --bs=4k --number_ios=64 --iodepth=8
    run -0 fio_result '.jobs[0].read.clat_ns.N'
    assert_output 64
}

@test "a job without statistics of the latency and the bandwidth reads all bytes" {
    fio_json --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=read --bs=64k --disable_clat=1 --disable_slat=1 --disable_bw_measurement=1
    run -0 fio_result '.jobs[0].read.io_bytes'
    assert_output 4194304
}
