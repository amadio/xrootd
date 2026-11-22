#!/usr/bin/env bash

# The path of a root:// URL. The xroot protocol sends the path after the slash
# which ends the host, thus root://host/dir/file has the relative path
# dir/file, which the server refuses. xrdcp makes it absolute, as xrdfs does
# (issue #3019). The server is shared by the tests, and each test has its own
# directory.

load ../setup/server.bash

setup_file() {
    start_server
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    DIR="t${BATS_TEST_NUMBER}"
    mkdir "$SERVER_DATA/$DIR"
    echo 'Hello, World' > hello.txt
}

@test "an upload to a URL with one slash writes the file" {
    run -0 xrdcp hello.txt "$SERVER_URL/$DIR/file.txt"
    run -0 cmp hello.txt "$SERVER_DATA/$DIR/file.txt"
}

@test "an upload into a directory of a URL with one slash writes the file" {
    run -0 xrdcp hello.txt "$SERVER_URL/$DIR/"
    run -0 cmp hello.txt "$SERVER_DATA/$DIR/hello.txt"
    rm "$SERVER_DATA/$DIR/hello.txt"
    run -0 xrdcp hello.txt "$SERVER_URL/$DIR"
    run -0 cmp hello.txt "$SERVER_DATA/$DIR/hello.txt"
}

@test "an upload into the top directory of a URL with one slash writes the file" {
    cp hello.txt "$DIR.txt"
    run -0 xrdcp "$DIR.txt" "$SERVER_URL/"
    run -0 cmp hello.txt "$SERVER_DATA/$DIR.txt"
}

@test "a download from a URL with one slash reads the file" {
    cp hello.txt "$SERVER_DATA/$DIR/file.txt"
    run -0 xrdcp "$SERVER_URL/$DIR/file.txt" -
    assert_output 'Hello, World'
}

@test "a recursive download from a URL with one slash reads the files" {
    mkdir "$SERVER_DATA/$DIR/sub"
    cp hello.txt "$SERVER_DATA/$DIR/sub/file.txt"
    mkdir copy
    run -0 xrdcp -r "$SERVER_URL/$DIR/sub" copy
    run -0 cmp hello.txt copy/sub/file.txt
}

@test "a URL with two slashes keeps its path" {
    run -0 xrdcp hello.txt "$SERVER_URL//$DIR/file.txt"
    run -0 cmp hello.txt "$SERVER_DATA/$DIR/file.txt"
}
