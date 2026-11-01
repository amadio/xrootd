#!/usr/bin/env bash

# The errors of the fio engine: the errno of fio for an error of XRootD, and
# the message of the server. The fault injection of libXrdOssTests fails
# reads of fail_read.txt after offset 0, and writes to no_space.txt after
# 2 MiB with ENOSPC. Each test has its own server.

load fio.bash

setup_file() {
    require_fio
    require_plugin libXrdOssTests
}

setup() {
    start_server faults.cfg
    make_random_file "$SERVER_DATA/fail_read.txt" 1048576
    cd "$BATS_TEST_TMPDIR"
}

teardown() {
    stop_servers
}

@test "a read of a missing file fails with ENOENT" {
    run -1 xfio --name=read --filename="$(fio_url "$SERVER_URL" missing.dat)" \
        --rw=read --bs=64k --size=1m --readonly
    assert_output --partial 'err=2/'
}

@test "a write to a file without write permission fails with EACCES" {
    make_random_file "$SERVER_DATA/readonly.dat" 1048576
    chmod 0444 "$SERVER_DATA/readonly.dat"
    run -1 xfio --name=write --filename="$(fio_url "$SERVER_URL" readonly.dat)" \
        --rw=write --bs=64k --size=1m
    assert_output --partial 'err=13/'
}

@test "a write to a file without write permission reports the error of the open" {
    make_random_file "$SERVER_DATA/readonly.dat" 1048576
    chmod 0444 "$SERVER_DATA/readonly.dat"
    run -1 xfio --name=write --filename="$(fio_url "$SERVER_URL" readonly.dat)" \
        --rw=write --bs=64k --size=1m
    assert_output --partial '[3010]'
}

@test "a failed read fails with EIO" {
    run -1 xfio --name=read --filename="$(fio_url "$SERVER_URL" fail_read.txt)" \
        --rw=read --bs=64k
    assert_output --partial 'err=5/'
}

@test "a failed read prints the error of the server" {
    run -1 xfio --name=read --filename="$(fio_url "$SERVER_URL" fail_read.txt)" \
        --rw=read --bs=64k
    assert_output --partial 'fio: [ERROR] Server responded with an error: [3007]'
}

@test "continue_on_error=read counts the failed reads" {
    fio_json --name=read --filename="$(fio_url "$SERVER_URL" fail_read.txt)" \
        --rw=read --bs=64k --continue_on_error=read
    # all reads but the first one, at offset 0
    run -0 fio_result '.jobs[0].total_err'
    assert_output 15
}

@test "a write after the space is full fails with ENOSPC" {
    run -1 xfio --name=write --filename="$(fio_url "$SERVER_URL" no_space.txt)" \
        --rw=write --bs=1m --size=4m
    assert_output --partial 'err=28/'
}

@test "ignore_error ignores ENOSPC of a write" {
    # the lists of the error types are separated with commas, and an empty
    # list of the reads ends the option
    run -0 fio_json --name=write --filename="$(fio_url "$SERVER_URL" no_space.txt)" \
        --rw=write --bs=1m --size=4m --continue_on_error=write --ignore_error=EIO,ENOSPC
}

@test "ignore_error counts the ignored ENOSPC of a write" {
    fio_json --name=write --filename="$(fio_url "$SERVER_URL" no_space.txt)" \
        --rw=write --bs=1m --size=4m --continue_on_error=write --ignore_error=EIO,ENOSPC
    # the write at offset 3 MiB, the only one after 2 MiB
    run -0 fio_result '.jobs[0].total_err'
    assert_output 1
}

@test "a job against a port without a server fails with ECONNREFUSED" {
    XRD_CONNECTIONRETRY=0 run -1 xfio --name=read --rw=read --bs=64k --size=1m \
        --filename="$(fio_url "root://localhost:$NO_SERVER_PORT" data.dat)"
    assert_output --partial 'err=111/'
}

@test "a file name which is not a URL fails with EINVAL" {
    run -1 xfio --name=read --filename='root\://' --rw=read --bs=64k --size=1m
    assert_output --partial 'fio: xrootd: invalid URL: root://'
}

@test "a layout which fails reports the error of the server" {
    run -1 xfio --name=read --filename="$(fio_url "$SERVER_URL" no_space.txt)" \
        --rw=read --bs=64k --size=4m
    assert_output --partial 'err=28/'
}

@test "a pre_read which fails reports the error of the server" {
    # the pre-read reads 1 MiB at a time, and only the first read is at 0
    make_random_file "$SERVER_DATA/fail_read.txt" 2097152
    run -1 xfio --name=read --filename="$(fio_url "$SERVER_URL" fail_read.txt)" \
        --rw=read --bs=64k --pre_read=1
    assert_output --partial 'fio: xrootd: pre-read of'
}
