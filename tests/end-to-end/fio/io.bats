#!/usr/bin/env bash

# Reads and writes of the fio engine on one data server: the size of the
# files, the layout of the files of a read job, and the verification of the
# data. Each test has its own server.

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

@test "a sequential write creates a file of the size of the job" {
    xfio --name=write --filename="$(fio_url "$SERVER_URL" new.dat)" \
        --rw=write --bs=1m --size=8m --iodepth=8
    run -0 file_size "$SERVER_DATA/new.dat"
    assert_output 8388608
}

@test "a write job writes all bytes of the job" {
    fio_json --name=write --filename="$(fio_url "$SERVER_URL" new.dat)" \
        --rw=randwrite --bs=64k --size=4m --iodepth=8
    run -0 fio_result '.jobs[0].write.io_bytes'
    assert_output 4194304
}

@test "a write creates the parent directories of the file" {
    xfio --name=write --filename="$(fio_url "$SERVER_URL" a/b/new.dat)" \
        --rw=write --bs=64k --size=1m
    run -0 file_size "$SERVER_DATA/a/b/new.dat"
    assert_output 1048576
}

@test "a read without size reads the whole file" {
    make_random_file "$SERVER_DATA/data.dat" 3145728
    fio_json --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randread --bs=64k --iodepth=8
    run -0 fio_result '.jobs[0].read.io_bytes'
    assert_output 3145728
}

@test "a read stops at the end of a file which is smaller than the size of the job" {
    make_random_file "$SERVER_DATA/data.dat" 1048576
    fio_json --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=read --bs=64k --size=4m --readonly
    run -0 fio_result '.jobs[0].read.io_bytes'
    assert_output 1048576
}

@test "a write with file_append writes after the end of the file" {
    make_random_file "$SERVER_DATA/data.dat" 1048576
    xfio --name=append --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=2m --file_append=1
    run -0 file_size "$SERVER_DATA/data.dat"
    assert_output 3145728
}

@test "a write with file_append keeps the data of the file" {
    make_random_file "$SERVER_DATA/data.dat" 1048576
    cp "$SERVER_DATA/data.dat" before.dat
    xfio --name=append --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --file_append=1
    run -0 cmp -n 1048576 before.dat "$SERVER_DATA/data.dat"
}

@test "a read job lays out a missing file" {
    xfio --name=read --filename="$(fio_url "$SERVER_URL" new.dat)" \
        --rw=randread --bs=64k --size=2m
    run -0 file_size "$SERVER_DATA/new.dat"
    assert_output 2097152
}

@test "a read job prints the layout of a missing file" {
    run -0 xfio --name=read --filename="$(fio_url "$SERVER_URL" new.dat)" \
        --rw=randread --bs=64k --size=2m
    assert_output --partial 'read: Laying out IO file (1 file / 2MiB)'
}

@test "a read job lays out only the missing end of a short file" {
    make_random_file "$SERVER_DATA/data.dat" 1048576
    cp "$SERVER_DATA/data.dat" before.dat
    xfio --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randread --bs=64k --size=2m
    run -0 cmp -n 1048576 before.dat "$SERVER_DATA/data.dat"
}

@test "jobs which share a file lay it out once" {
    run -0 xfio --name=read --filename="$(fio_url "$SERVER_URL" new.dat)" \
        --rw=randread --bs=64k --size=2m --numjobs=3
    assert_equal "$(grep -c 'Laying out' <<< "$output")" 1
}

@test "a read job reads the data of the layout" {
    fio_json --name=read --filename="$(fio_url "$SERVER_URL" new.dat)" \
        --rw=read --bs=64k --size=2m
    run -0 fio_result '.jobs[0].read.io_bytes'
    assert_output 2097152
}

@test "a read job after a write job of the same file reads the data of the write" {
    run -0 xfio --filename="$(fio_url "$SERVER_URL" data.dat)" --bs=64k --size=2m \
        --verify=crc32c --verify_fatal=1 \
        --name=write --rw=write --do_verify=0 \
        --name=read --stonewall --rw=read
}

@test "a read job with readonly refuses the layout of a missing file" {
    run -1 xfio --name=read --filename="$(fio_url "$SERVER_URL" new.dat)" \
        --rw=randread --bs=64k --size=1m --readonly
    assert_output --partial 'fio: refusing extend of file due to read-only'
}

@test "a mixed random read and write completes without errors" {
    fio_json --name=randrw --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randrw --rwmixread=70 --bs=64k --size=4m --iodepth=8
    run -0 fio_result '.jobs[0].error'
    assert_output 0
}

@test "a verified write reads back the data which it wrote" {
    run -0 xfio --name=verify --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=4m --iodepth=8 \
        --verify=crc32c --do_verify=1 --verify_fatal=1
}

@test "verify_only verifies the data of an earlier write" {
    xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=4m --verify=md5 --do_verify=0
    run -0 xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=4m --verify=md5 --verify_only=1 --verify_fatal=1
}

@test "verify_only finds a change of the data" {
    xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --verify=md5 --do_verify=0
    printf 'changed' | dd of="$SERVER_DATA/data.dat" bs=1 seek=4096 conv=notrunc status=none
    run -1 xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --verify=md5 --verify_only=1 --verify_fatal=1
    assert_output --partial 'md5: verify failed at file'
}

@test "pre_read reads the files before the job" {
    make_random_file "$SERVER_DATA/data.dat" 1048576
    run -0 xfio --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=read --bs=64k --pre_read=1
}

@test "unlink removes the file after the job" {
    xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --unlink=1
    run ! test -e "$SERVER_DATA/data.dat"
}

@test "a write without xrd_recreate keeps the size of a larger file" {
    make_random_file "$SERVER_DATA/data.dat" 4194304
    xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m
    run -0 file_size "$SERVER_DATA/data.dat"
    assert_output 4194304
}

@test "xrd_recreate replaces a larger file" {
    make_random_file "$SERVER_DATA/data.dat" 4194304
    xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --xrd_recreate=1
    run -0 file_size "$SERVER_DATA/data.dat"
    assert_output 1048576
}

@test "xrd_posc keeps the file of a job which succeeds" {
    xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=1m --xrd_posc=1
    run -0 file_size "$SERVER_DATA/data.dat"
    assert_output 1048576
}

@test "an iolog replays its reads" {
    make_random_file "$SERVER_DATA/data.dat" 1048576
    xfio --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randread --bs=4k --number_ios=100 --write_iolog=reads.log
    fio_json --name=replay --read_iolog=reads.log
    run -0 fio_result '.jobs[0].read.total_ios'
    assert_output 100
}

@test "a read past the end of the file reads the data up to the end" {
    make_random_file "$SERVER_DATA/data.dat" 1000
    short_read_log "$(fio_url "$SERVER_URL" data.dat)" > reads.log
    fio_json --name=replay --read_iolog=reads.log
    run -0 fio_result '.jobs[0].read.io_bytes'
    assert_output 1000
}

# At the end of an iolog, fio closes the file with the read past the end of
# the file still in flight
@test "an iolog which ends with a read in flight completes without errors" {
    make_random_file "$SERVER_DATA/short.dat" 1000
    end_read_log "$(fio_url "$SERVER_URL" short.dat)" > reads.log
    fio_json --name=replay --read_iolog=reads.log --iodepth=2
    run -0 fio_result '.jobs[0].error'
    assert_output 0
}

@test "pre_read in a write job of a missing file writes the file" {
    xfio --name=write --filename="$(fio_url "$SERVER_URL" new.dat)" \
        --rw=write --bs=64k --size=1m --pre_read=1
    run -0 file_size "$SERVER_DATA/new.dat"
    assert_output 1048576
}
