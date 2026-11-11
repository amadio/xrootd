#!/usr/bin/env bash

# A checksum gives information about the content of a file, and the server
# reads the file to calculate it. Thus, a checksum query needs the read
# privilege; the lookup privilege, which is sufficient for a stat, is not.

load ../setup/server.bash

setup_file() {
    start_server checksum.cfg authz.cfg

    mkdir -p "$SERVER_DATA/listed"
    echo 'content' > "$SERVER_DATA/readable.dat"
    echo 'content' > "$SERVER_DATA/listed/file.dat"
}

teardown_file() {
    stop_servers
}

@test "a checksum query with the read privilege succeeds" {
    run -0 xrdfs "$SERVER_URL" query checksum /readable.dat
    assert_output "adler32 $(checksum adler32 "$SERVER_DATA/readable.dat")"
}

@test "a stat with only the lookup privilege succeeds" {
    run -0 xrdfs "$SERVER_URL" stat /listed/file.dat
}

@test "a checksum query with only the lookup privilege is not authorized" {
    run ! xrdfs "$SERVER_URL" query checksum /listed/file.dat
    assert_output --partial '[3010]'
}
