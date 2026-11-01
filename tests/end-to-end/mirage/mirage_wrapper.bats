#!/usr/bin/env bash

# The Mirage OSS stacked on the default OSS: files below /mirage_test/ keep
# only their size, and the other files go to the disk. Each test has its own
# server.

load ../setup/server.bash

setup() {
    start_server mirage_wrapper.cfg

    # workdir as test tmp dir (all files are removed after execution)
    cd "$BATS_TEST_TMPDIR"

    printf 'stored on disk' > local.txt
}

teardown() {
    stop_servers
}

# --- writing: disk vs memory ------------------------------------------------

@test "file written outside the mirage path is stored on disk" {
    run -0 xrdcp -f local.txt "$SERVER_URL//diskfile"

    assert [ -f "$SERVER_DATA/diskfile" ]
    assert_equal "$(cat "$SERVER_DATA/diskfile")" 'stored on disk'
}

@test "file written under the mirage path is not stored on disk" {
    echo 'discarded content' > local.txt

    run -0 xrdcp -f local.txt "$SERVER_URL//mirage_test/memfile"

    assert [ ! -e "$SERVER_DATA/mirage_test/memfile" ]
}

# --- reading: disk vs memory ------------------------------------------------

@test "file read from outside the mirage path returns the exact written content" {
    run -0 xrdcp -f local.txt "$SERVER_URL//diskfile"

    run -0 xrdcp -f "$SERVER_URL//diskfile" -
    assert_output 'stored on disk'
}

@test "file read from under the mirage path is served from memory at the retained size" {
    run -0 xrdcp -f local.txt "$SERVER_URL//mirage_test/memfile"

    # the download succeeds and yields the retained size even though the bytes
    # were discarded and nothing was ever written to disk
    run -0 xrdcp -f "$SERVER_URL//mirage_test/memfile" downloaded.txt
    assert_equal "$(wc -c < downloaded.txt | tr -d ' ')" 14
    assert [ ! -e "$SERVER_DATA/mirage_test/memfile" ]
}

@test "file size is retained under the mirage path even though its content is discarded" {
    run -0 xrdcp -f local.txt "$SERVER_URL//mirage_test/memfile"

    run -0 xrdfs "$SERVER_URL" stat /mirage_test/memfile
    assert_output --partial 'Size:   14'
}

# --- reading directories: disk vs memory ------------------------------------

@test "a directory outside the mirage path can be listed" {
    run -0 xrdcp -f local.txt "$SERVER_URL//diskfile"

    run -0 xrdfs "$SERVER_URL" ls /
    assert_output --partial '/diskfile'
}

@test "listing a directory under the mirage path returns an error" {
    run ! xrdfs "$SERVER_URL" ls /mirage_test/
    assert_output --partial '/mirage_test/'
}
