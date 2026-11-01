#!/usr/bin/env bash

# Page reads and writes of the fio engine (xrd_pgio=1): kXR_pgread and
# kXR_pgwrite, with a CRC32C checksum for each page. Each test has its own
# server.

load fio.bash

setup_file() {
    require_fio
}

setup() {
    start_server
    cd "$BATS_TEST_TMPDIR"
}

teardown() {
    stop_servers
}

@test "page writes and page reads verify the data" {
    run -0 xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=4m --iodepth=8 --xrd_pgio=1 \
        --verify=crc32c --do_verify=1 --verify_fatal=1
}

@test "page writes send kXR_pgwrite" {
    XRD_LOGLEVEL=Debug xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --xrd_pgio=1
    run -0 requests kXR_pgwrite
    assert_output 16
}

@test "page reads send kXR_pgread" {
    make_random_file "$SERVER_DATA/data.dat" 1048576
    XRD_LOGLEVEL=Debug xfio --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=read --bs=64k --xrd_pgio=1
    run -0 requests kXR_pgread
    assert_output 16
}

@test "page reads verify the data of plain writes" {
    xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=4m --verify=crc32c --do_verify=0
    run -0 xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=4m --verify=crc32c --verify_only=1 \
        --verify_fatal=1 --iodepth=8 --xrd_pgio=1
}

@test "page reads of a whole file read all bytes" {
    make_random_file "$SERVER_DATA/data.dat" 4194304
    fio_json --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=read --bs=64k --xrd_pgio=1
    run -0 fio_result '.jobs[0].read.io_bytes'
    assert_output 4194304
}

@test "a page read past the end of the file reads the data up to the end" {
    make_random_file "$SERVER_DATA/data.dat" 1000
    short_read_log "$(fio_url "$SERVER_URL" data.dat)" > reads.log
    fio_json --name=replay --read_iolog=reads.log --xrd_pgio=1
    run -0 fio_result '.jobs[0].read.io_bytes'
    assert_output 1000
}
