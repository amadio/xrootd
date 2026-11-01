#!/usr/bin/env bash

# Benchmarks of the client through a cache (XCache) in front of an origin:
# the bandwidth, the CPU time of the client, the cache and the origin, and,
# for a cache which starts without the file, the read amplification of the
# cache: the bytes of the origin for each byte which the client reads. Each
# benchmark has its own cache, which starts empty, with the block size of the
# benchmark, and its own collector of the g-stream of the cache. CTest runs
# each benchmark alone, as the test benchmark::cache::<name>.

load benchmark.bash
load ../end-to-end/cache/cache.bash

setup_file() {
    require_fio
    start_server origin
    make_random_file "$ORIGIN_DATA/data.dat" $(( 256 * 1024 * 1024 ))
    export ORIGIN="localhost:$ORIGIN_PORT"
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    export BENCHMARK_SERVERS="cache origin"
}

teardown() {
    stop_servers
}

# start_bench_cache <block size> [prefetch]
#
# Start an empty cache with the block size and the number of blocks to
# prefetch (default 0). Write the iologs of the analysis with ROOT: root.log
# reads the file once, root8.log 8 times.
# The collector of its g-stream gives the read amplification of a job of a
# cold cache (see bench).
start_bench_cache() {
    export PFC_BLOCKSIZE=$1 PFC_PREFETCH=${2:-0}
    start_collector gstream
    start_server cache cache pfc.cfg "$GSTREAM_CFG"
    DATA=$(fio_url "$CACHE_URL" data.dat)
    # 24 clusters of 9.8 MiB, which fit in the file of 256 MiB
    root_iolog "$CACHE_URL//data.dat" 24 > root.log
    root_iolog "$CACHE_URL//data.dat" 24 8 > root8.log
}

@test "cache-read-seq-1m-cold" {
    start_bench_cache 1m
    export BENCHMARK_GSTREAM=$GSTREAM_RECORDS BENCHMARK_LFN=/data.dat
    bench --name=read --filename="$DATA" --rw=read --bs=1m --iodepth=16
}

@test "cache-read-seq-1m-warm" {
    start_bench_cache 1m
    xfio --name=warm --filename="$DATA" --rw=read --bs=1m --iodepth=16 > /dev/null
    bench --name=read --filename="$DATA" --rw=read --bs=1m --iodepth=16 "${TIMED[@]}"
}

@test "cache-read-rand-4k-warm" {
    start_bench_cache 1m
    xfio --name=warm --filename="$DATA" --rw=read --bs=1m --iodepth=16 > /dev/null
    bench --name=read --filename="$DATA" --rw=randread --bs=4k --iodepth=32 "${TIMED[@]}"
}

@test "cache-root-analysis-64k" {
    start_bench_cache 64k
    export BENCHMARK_GSTREAM=$GSTREAM_RECORDS BENCHMARK_LFN=/data.dat
    bench --name=root --read_iolog=root.log --replay_no_stall=1 --iodepth=10 \
        --iodepth_batch=10 --xrd_readv=1
}

@test "cache-root-analysis-128k" {
    start_bench_cache 128k
    export BENCHMARK_GSTREAM=$GSTREAM_RECORDS BENCHMARK_LFN=/data.dat
    bench --name=root --read_iolog=root.log --replay_no_stall=1 --iodepth=10 \
        --iodepth_batch=10 --xrd_readv=1
}

@test "cache-root-analysis-512k" {
    start_bench_cache 512k
    export BENCHMARK_GSTREAM=$GSTREAM_RECORDS BENCHMARK_LFN=/data.dat
    bench --name=root --read_iolog=root.log --replay_no_stall=1 --iodepth=10 \
        --iodepth_batch=10 --xrd_readv=1
}

@test "cache-root-analysis-1m" {
    start_bench_cache 1m
    export BENCHMARK_GSTREAM=$GSTREAM_RECORDS BENCHMARK_LFN=/data.dat
    bench --name=root --read_iolog=root.log --replay_no_stall=1 --iodepth=10 \
        --iodepth_batch=10 --xrd_readv=1
}

@test "cache-root-analysis-1m-prefetch" {
    start_bench_cache 1m 8
    export BENCHMARK_GSTREAM=$GSTREAM_RECORDS BENCHMARK_LFN=/data.dat
    bench --name=root --read_iolog=root.log --replay_no_stall=1 --iodepth=10 \
        --iodepth_batch=10 --xrd_readv=1
}

@test "cache-root-analysis-1m-warm" {
    start_bench_cache 1m
    xfio --name=warm --read_iolog=root.log --replay_no_stall=1 --iodepth=10 \
        --iodepth_batch=10 --xrd_readv=1 > /dev/null
    bench --name=root --read_iolog=root8.log --replay_no_stall=1 --iodepth=10 \
        --iodepth_batch=10 --xrd_readv=1
}
