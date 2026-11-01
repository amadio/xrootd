#!/usr/bin/env bash

# The fio engine through a cache (XCache) in front of an origin. The cache
# fetches whole blocks of 128 KiB from the origin, without prefetch. The
# file_close record of the g-stream of the cache tells how many bytes came
# from the origin (b_todisk). The tests share the origin, the cache and the
# collector; each test has its own file.

load ../cache/cache.bash
load fio.bash

setup_file() {
    require_fio
    start_cache
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    F="t${BATS_TEST_NUMBER}.dat"
}

@test "a read through the cache returns the data of the origin" {
    xfio --name=write --filename="$(fio_url "$ORIGIN_URL" "$F")" \
        --rw=write --bs=64k --size=4m --verify=crc32c --do_verify=0
    run -0 xfio --name=write --filename="$(fio_url "$CACHE_URL" "$F")" \
        --rw=randwrite --bs=64k --size=4m --verify=crc32c --verify_only=1 \
        --verify_fatal=1 --iodepth=8
}

@test "a sparse read through the cache fetches a whole block for each read" {
    make_origin_file "$F" 32
    # 4 KiB at the start of each block of 128 KiB; after the end of the
    # file, a read with holes starts again at offset 0
    xfio --name=read --filename="$(fio_url "$CACHE_URL" "$F")" \
        --rw=read:124k --bs=4k --number_ios=32 --readonly
    run -0 close_field "$F" 1 b_todisk
    assert_output $((32 * BLOCK))
}

@test "a sparse read through the cache reads only the bytes of the job" {
    make_origin_file "$F" 32
    fio_json --name=read --filename="$(fio_url "$CACHE_URL" "$F")" \
        --rw=read:124k --bs=4k --number_ios=32 --readonly
    run -0 fio_result '.jobs[0].read.io_bytes'
    assert_output $((32 * 4096))
}

# The job reads 4 of the 32 blocks. The open of the job can come before the
# cache closes the file of the pre-read, and then both are one access, thus
# the test adds up the bytes of all accesses.
@test "pre_read through the cache fetches the whole file" {
    make_origin_file "$F" 32
    xfio --name=read --filename="$(fio_url "$CACHE_URL" "$F")" \
        --rw=read:124k --bs=4k --number_ios=4 --readonly --pre_read=1
    run -0 fetched_total "$F"
    assert_output $((32 * BLOCK))
}

@test "vector reads through the cache return the data of the origin" {
    xfio --name=write --filename="$(fio_url "$ORIGIN_URL" "$F")" \
        --rw=write --bs=64k --size=4m --verify=crc32c --do_verify=0
    run -0 xfio --name=write --filename="$(fio_url "$CACHE_URL" "$F")" \
        --rw=randwrite --bs=64k --size=4m --verify=crc32c --verify_only=1 \
        --verify_fatal=1 --iodepth=16 --iodepth_batch=16 --xrd_readv=1
}

@test "sparse vector reads through the cache fetch a whole block for each read" {
    make_origin_file "$F" 32
    xfio --name=read --filename="$(fio_url "$CACHE_URL" "$F")" \
        --rw=read:124k --bs=4k --number_ios=32 --readonly --iodepth=8 --iodepth_batch=8 \
        --xrd_readv=1
    run -0 close_field "$F" 1 b_todisk
    assert_output $((32 * BLOCK))
}
