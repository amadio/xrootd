#!/usr/bin/env bash

# Jobs of the fio engine with several files: a list in filename=, or a
# template in filename_format= with nrfiles=. Each file has its own
# XrdCl::File. Each test has its own server.

load fio.bash

setup_file() {
    require_fio
}

setup() {
    start_server
    cd "$BATS_TEST_TMPDIR"
    # filename_format is not split on ':', thus its colons have no backslash
    FORMAT="$SERVER_URL//files/f.\$jobnum.\$filenum"
}

teardown() {
    stop_servers
}

@test "filename_format with nrfiles creates all files" {
    xfio --name=write --filename_format="$FORMAT" --nrfiles=4 \
        --rw=write --bs=64k --size=4m --iodepth=8
    run -0 ls "$SERVER_DATA/files"
    assert_output "$(printf 'f.0.0\nf.0.1\nf.0.2\nf.0.3')"
}

@test "filename_format with nrfiles divides the size among the files" {
    xfio --name=write --filename_format="$FORMAT" --nrfiles=4 \
        --rw=write --bs=64k --size=4m --iodepth=8
    run -0 file_size "$SERVER_DATA/files/f.0.3"
    assert_output 1048576
}

@test "filename_format with escaped colons names the same files" {
    xfio --name=write --filename_format="$FORMAT" --nrfiles=4 \
        --rw=write --bs=64k --size=4m
    fio_json --name=read --filename_format="${FORMAT//:/\\:}" --nrfiles=4 \
        --rw=randread --bs=64k --size=4m --readonly
    run -0 fio_result '.jobs[0].read.io_bytes'
    assert_output 4194304
}

@test "a list of URLs in filename writes each file" {
    xfio --name=write --rw=write --bs=64k --size=2m \
        --filename="$(fio_url "$SERVER_URL" a.dat):$(fio_url "$SERVER_URL" b.dat)"
    run -0 file_size "$SERVER_DATA/b.dat"
    assert_output 1048576
}

@test "filesize gives each file its size" {
    xfio --name=write --filename_format="$FORMAT" --nrfiles=8 --filesize=8k \
        --rw=write --bs=4k
    run -0 file_size "$SERVER_DATA/files/f.0.7"
    assert_output 8192
}

@test "a job with more files than openfiles writes all files" {
    xfio --name=write --filename_format="$FORMAT" --nrfiles=50 --filesize=4k \
        --openfiles=4 --rw=write --bs=4k
    run -0 bash -c "ls '$SERVER_DATA/files' | wc -l"
    assert_output 50
}

@test "a job with more files than openfiles reads all files" {
    fio_json --name=read --filename_format="$FORMAT" --nrfiles=50 --filesize=4k \
        --openfiles=4 --file_service_type=sequential --rw=randread --bs=4k
    run -0 fio_result '.jobs[0].read.io_bytes'
    assert_output 204800
}

@test "a read job of a list of files lays out only the missing files" {
    make_random_file "$SERVER_DATA/a.dat" 1048576
    run -0 xfio --name=read --rw=read --bs=64k --size=2m \
        --filename="$(fio_url "$SERVER_URL" a.dat):$(fio_url "$SERVER_URL" b.dat)"
    assert_output --partial 'read: Laying out IO file (1 file / 1MiB)'
}
