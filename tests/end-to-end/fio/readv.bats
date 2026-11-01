#!/usr/bin/env bash

# Vector reads of the fio engine (xrd_readv=1): the engine sends the reads
# which fio queues as one kXR_readv, within the limits of the server. fio
# queues iodepth_batch reads before it commits them. Each test has its own
# server.

load fio.bash

setup_file() {
    require_fio
}

setup() {
    start_server
    cd "$BATS_TEST_TMPDIR"
    xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=write --bs=64k --size=8m --verify=crc32c --do_verify=0
}

teardown() {
    stop_servers
}

@test "vector reads return the data of the file" {
    run -0 xfio --name=write --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randwrite --bs=64k --size=8m --verify=crc32c --verify_only=1 \
        --verify_fatal=1 --iodepth=16 --iodepth_batch=16 --xrd_readv=1
}

@test "a vector read has all reads of a batch" {
    XRD_LOGLEVEL=Debug xfio --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randread --bs=64k --number_ios=16 --iodepth=16 --iodepth_batch=16 \
        --xrd_readv=1
    run -0 requests kXR_readv
    assert_output 1
}

@test "a vector read sends no plain read" {
    XRD_LOGLEVEL=Debug xfio --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randread --bs=64k --number_ios=16 --iodepth=16 --iodepth_batch=16 \
        --xrd_readv=1
    run -0 requests kXR_read
    assert_output 0
}

@test "a batch of more reads than the server allows in one vector read is split" {
    # the server allows 1024 segments in one kXR_readv
    XRD_LOGLEVEL=Debug xfio --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=read --bs=4k --iodepth=2048 --iodepth_batch=2048 --xrd_readv=1
    run -0 requests kXR_readv
    assert_output 2
}

@test "a split batch returns the data of the file" {
    # 2048 reads of 4 KiB, thus two kXR_readv of 1024 segments
    xfio --name=write --filename="$(fio_url "$SERVER_URL" small.dat)" \
        --rw=write --bs=4k --size=8m --verify=crc32c --do_verify=0
    run -0 xfio --name=write --filename="$(fio_url "$SERVER_URL" small.dat)" \
        --rw=write --bs=4k --size=8m --verify=crc32c --verify_only=1 \
        --verify_fatal=1 --iodepth=2048 --iodepth_batch=2048 --xrd_readv=1
}

@test "a read larger than a segment of a vector read is a plain read" {
    # the server allows segments of 2 MiB - 16 bytes
    XRD_LOGLEVEL=Debug xfio --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=read --bs=4m --iodepth=2 --iodepth_batch=2 --xrd_readv=1
    run -0 requests kXR_read
    assert_output 2
}

@test "a read larger than a segment of a vector read returns the data of the file" {
    xfio --name=write --filename="$(fio_url "$SERVER_URL" big.dat)" \
        --rw=write --bs=4m --size=8m --verify=crc32c --do_verify=0
    run -0 xfio --name=write --filename="$(fio_url "$SERVER_URL" big.dat)" \
        --rw=write --bs=4m --size=8m --verify=crc32c --verify_only=1 \
        --verify_fatal=1 --iodepth=2 --iodepth_batch=2 --xrd_readv=1
}

@test "the engine asks the data server for its limits of vector reads" {
    XRD_LOGLEVEL=Debug xfio --name=read --filename="$(fio_url "$SERVER_URL" data.dat)" \
        --rw=randread --bs=64k --number_ios=16 --xrd_readv=1
    run -0 requests kXR_query
    assert_output 1
}

@test "the engine asks a data server once for its limits of vector reads" {
    xfio --name=write --filename="$(fio_url "$SERVER_URL" second.dat)" \
        --rw=write --bs=64k --size=1m
    XRD_LOGLEVEL=Debug xfio --name=read --bs=64k --number_ios=16 --xrd_readv=1 \
        --filename="$(fio_url "$SERVER_URL" data.dat):$(fio_url "$SERVER_URL" second.dat)" \
        --rw=randread
    run -0 requests kXR_query
    assert_output 1
}

# The server fails a whole kXR_readv with a segment past the end of the file
@test "a batch with a read past the end of the file completes without errors" {
    make_random_file "$SERVER_DATA/short.dat" 1000
    end_read_log "$(fio_url "$SERVER_URL" short.dat)" > reads.log
    fio_json --name=replay --read_iolog=reads.log --iodepth=2 --iodepth_batch=2 \
        --xrd_readv=1
    run -0 fio_result '.jobs[0].error'
    assert_output 0
}
