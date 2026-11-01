#!/usr/bin/env bash

# The fio engine against a cluster: a manager and two data servers. A job
# names the files with the URL of the manager, which redirects each file to
# a data server. The tests share the cluster; each test has its own files.

load ../setup/cluster.bash
load fio.bash

setup_file() {
    require_fio
    start_manager man ""
    start_data_server srv1 man
    start_data_server srv2 man
    wait_for_cluster man 2
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    D="t$BATS_TEST_NUMBER"
    FORMAT="$MAN_URL//$D/f.\$filenum"
}

@test "a job through the manager writes all files on the data servers" {
    xfio --name=write --filename_format="$FORMAT" --nrfiles=8 --filesize=64k \
        --rw=write --bs=64k
    run -0 bash -c "ls '$SRV1_DATA/$D' '$SRV2_DATA/$D' 2>/dev/null | grep -c '^f\.'"
    assert_output 8
}

@test "a read through the manager returns the data of the data servers" {
    xfio --name=write --filename_format="$FORMAT" --nrfiles=4 --filesize=1m \
        --rw=write --bs=64k --verify=crc32c --do_verify=0
    run -0 xfio --name=write --filename_format="$FORMAT" --nrfiles=4 --filesize=1m \
        --rw=write --bs=64k --verify=crc32c --verify_only=1 --verify_fatal=1
}

@test "vector reads through the manager ask the data server for its limits" {
    xfio --name=write --filename_format="$FORMAT" --nrfiles=1 --filesize=1m \
        --rw=write --bs=64k
    XRD_LOGLEVEL=Debug xfio --name=read --filename_format="$FORMAT" --nrfiles=1 \
        --rw=randread --bs=4k --number_ios=16 --xrd_readv=1
    # the redirect names the data server by its address, for example [::1]
    run -0 grep -cE "\[.*:($SRV1_PORT|$SRV2_PORT)\] MsgHandler created: .*message: kXR_query " "$XRD_LOGFILE"
    assert_output 1
}

@test "vector reads through the manager return the data of the data servers" {
    xfio --name=write --filename_format="$FORMAT" --nrfiles=2 --filesize=1m \
        --rw=write --bs=64k --verify=crc32c --do_verify=0
    run -0 xfio --name=write --filename_format="$FORMAT" --nrfiles=2 --filesize=1m \
        --rw=randwrite --bs=64k --verify=crc32c --verify_only=1 --verify_fatal=1 \
        --iodepth=8 --iodepth_batch=8 --xrd_readv=1
}
