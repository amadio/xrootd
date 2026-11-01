#!/usr/bin/env bash

# Benchmarks of the client and one data server. The data are in a file of
# 256 MiB, which the page cache of the host holds, thus the benchmarks
# measure XRootD rather than the disk. CTest runs each benchmark alone, as
# the test benchmark::server::<name>.

load benchmark.bash

setup_file() {
    require_fio
    start_server
    make_random_file "$SERVER_DATA/data.dat" $(( 256 * 1024 * 1024 ))
    # the analysis with ROOT: 8 passes over 24 clusters of 9.8 MiB
    root_iolog "$SERVER_URL//data.dat" 24 8 > "$BATS_FILE_TMPDIR/root.log"
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    DATA=$(fio_url "$SERVER_URL" data.dat)
    NEW=$(fio_url "$SERVER_URL" "new$BATS_TEST_NUMBER.dat")
}

@test "read-seq-1m" {
    bench --name=read --filename="$DATA" --rw=read --bs=1m --iodepth=16 "${TIMED[@]}"
}

@test "read-seq-1m-4jobs" {
    bench --name=read --filename="$DATA" --rw=read --bs=1m --iodepth=16 --numjobs=4 "${TIMED[@]}"
}

@test "read-seq-1m-pgio" {
    bench --name=read --filename="$DATA" --rw=read --bs=1m --iodepth=16 --xrd_pgio=1 "${TIMED[@]}"
}

@test "read-rand-4k" {
    bench --name=read --filename="$DATA" --rw=randread --bs=4k --iodepth=32 "${TIMED[@]}"
}

@test "read-rand-4k-depth1" {
    bench --name=read --filename="$DATA" --rw=randread --bs=4k --iodepth=1 "${TIMED[@]}"
}

@test "read-rand-64k-readv" {
    bench --name=read --filename="$DATA" --rw=randread --bs=64k --iodepth=32 \
        --iodepth_batch=32 --xrd_readv=1 "${TIMED[@]}"
}

@test "read-root-analysis" {
    bench --name=root --read_iolog="$BATS_FILE_TMPDIR/root.log" --replay_no_stall=1 \
        --iodepth=10
}

@test "read-root-analysis-readv" {
    bench --name=root --read_iolog="$BATS_FILE_TMPDIR/root.log" --replay_no_stall=1 \
        --iodepth=10 --iodepth_batch=10 --xrd_readv=1
}

@test "read-small-files" {
    local format="$SERVER_URL//small$BATS_TEST_NUMBER/f.\$filenum"
    # the layout of the files is not part of the benchmark
    xfio --name=layout --filename_format="$format" --nrfiles=500 --filesize=16k \
        --rw=write --bs=16k > /dev/null
    bench --name=read --filename_format="$format" --nrfiles=500 --filesize=16k \
        --rw=read --bs=16k --openfiles=1 --file_service_type=sequential "${TIMED[@]}"
}

@test "write-seq-1m" {
    bench --name=write --filename="$NEW" --rw=write --bs=1m --size=256m --iodepth=16 "${TIMED[@]}"
}

@test "write-seq-1m-pgio" {
    bench --name=write --filename="$NEW" --rw=write --bs=1m --size=256m --iodepth=16 \
        --xrd_pgio=1 "${TIMED[@]}"
}

@test "write-rand-4k" {
    bench --name=write --filename="$NEW" --rw=randwrite --bs=4k --size=256m --iodepth=32 "${TIMED[@]}"
}

@test "rw-rand-64k" {
    bench --name=rw --filename="$DATA" --rw=randrw --rwmixread=70 --bs=64k --iodepth=16 "${TIMED[@]}"
}
