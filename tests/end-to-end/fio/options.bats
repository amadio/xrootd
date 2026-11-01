#!/usr/bin/env bash

# The native options of fio which the engine maps to XRootD: syncs, the
# creation of files, and the options which XRootD cannot do. Each test has
# its own server.

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

@test "fsync syncs the file after each group of writes" {
    fio_json --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --fsync=4
    # 16 writes; fio does not sync after the last group, as the job ends
    run -0 fio_result '.jobs[0].sync.total_ios'
    assert_output 3
}

@test "sync=1 syncs the file after each write" {
    XRD_LOGLEVEL=Debug xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randwrite --bs=64k --size=1m --iodepth=4 --sync=1
    run -0 requests kXR_sync
    assert_output 16
}

@test "sync=1 writes all bytes of the job" {
    fio_json --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randwrite --bs=64k --size=1m --iodepth=4 --sync=1
    run -0 fio_result '.jobs[0].write.io_bytes'
    assert_output 1048576
}

@test "end_fsync syncs the file once at the end of the job" {
    XRD_LOGLEVEL=Debug xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --end_fsync=1
    run -0 requests kXR_sync
    assert_output 1
}

@test "fsync_on_close syncs the file once before it closes" {
    XRD_LOGLEVEL=Debug xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --fsync_on_close=1
    run -0 requests kXR_sync
    assert_output 1
}

@test "a write without a sync option sends no sync" {
    XRD_LOGLEVEL=Debug xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m
    run -0 requests kXR_sync
    assert_output 0
}

@test "allow_file_create=0 refuses to create a missing file" {
    run -1 xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --allow_file_create=0
    assert_output --partial 'fio: file creation disallowed by allow_file_create=0'
}

@test "allow_file_create=0 does not create the file" {
    run -1 xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --allow_file_create=0
    run ! test -e "$SERVER_DATA/data.dat"
}

@test "allow_file_create=0 writes to an existing file" {
    make_random_file "$SERVER_DATA/data.dat" 1048576
    run -0 xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --allow_file_create=0
}

@test "allow_file_create=0 refuses the layout of a missing file" {
    run -1 xfio --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=read --bs=64k --size=1m --allow_file_create=0
    assert_output --partial 'fio: file creation disallowed by allow_file_create=0'
}

@test "a trim job fails with EOPNOTSUPP" {
    make_random_file "$SERVER_DATA/data.dat" 1048576
    run -1 xfio --name=trim --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randtrim --bs=64k
    assert_output --partial 'err=95'
}

@test "a trim of trim_percentage fails with EOPNOTSUPP" {
    run -1 xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --verify=md5 --trim_percentage=50 \
        --trim_backlog=1
    assert_output --partial 'err=95/'
}

@test "direct=1 prints that it has no effect" {
    run -0 xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --direct=1
    assert_output --partial 'fio: xrootd: direct=1 has no effect'
}

@test "xrd_workerthreads sets the number of XrdCl worker threads" {
    XRD_LOGLEVEL=Debug xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --xrd_workerthreads=7
    run -0 grep -c 'Job manager started, 7 workers' "$XRD_LOGFILE"
    assert_output 1
}

@test "xrd_recreate with allow_file_create=0 does not create a missing file" {
    run -1 xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --allow_file_create=0 --xrd_recreate=1
    run ! test -e "$SERVER_DATA/data.dat"
}

@test "xrd_recreate with allow_file_create=0 truncates an existing file" {
    make_random_file "$SERVER_DATA/data.dat" 4194304
    xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --allow_file_create=0 --xrd_recreate=1
    run -0 file_size "$SERVER_DATA/data.dat"
    assert_output 1048576
}
