#!/usr/bin/env bash

# The answers of the handler XrdHttpS3 to the S3 request ListObjectsV2, which
# XrdClS3 does not send: invalid requests, a bucket without a directory, and
# the access to a directory which the server cannot read. The server is
# shared by the tests, and each test has its own directory.

load ../setup/server.bash

readonly BUCKET=test-bucket

setup_file() {
    require_plugin libXrdHttp
    require_plugin libXrdHttpS3
    require_commands curl
    start_server server http.cfg handler.cfg
    mkdir "$SERVER_DATA/$BUCKET"
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    DIR="t${BATS_TEST_NUMBER}"
    mkdir "$SERVER_DATA/$BUCKET/$DIR"
}

# list <bucket and query>: print the HTTP status, and keep the body in body.xml
list() {
    curl --silent -o body.xml --write-out '%{http_code}' "$SERVER_HTTP/$1"
}

@test "a listing has the keys of the prefix" {
    : > "$SERVER_DATA/$BUCKET/$DIR/obj"
    run -0 list "$BUCKET?list-type=2&delimiter=/&prefix=$DIR/"
    assert_output 200
    run -0 cat body.xml
    assert_output --partial "<KeyCount>2</KeyCount>"
    assert_output --partial "<Key>$DIR/obj</Key>"
}

# XrdHttp decodes the query before it splits it, and the handler does not
@test "a prefix with an encoded '&' lists the keys which start with it" {
    : > "$SERVER_DATA/$BUCKET/$DIR/a&b"
    : > "$SERVER_DATA/$BUCKET/$DIR/a"
    run -0 list "$BUCKET?list-type=2&delimiter=/&prefix=$DIR/a%26"
    assert_output 200
    run -0 cat body.xml
    assert_output --partial "<KeyCount>1</KeyCount>"
    assert_output --partial "<Key>$DIR/a&amp;b</Key>"
}

@test "a prefix of a directory which does not exist has no keys" {
    run -0 list "$BUCKET?list-type=2&delimiter=/&prefix=$DIR/missing/"
    assert_output 200
    run -0 cat body.xml
    assert_output --partial "<KeyCount>0</KeyCount>"
}

@test "a GET of an object is not a listing" {
    printf 'data' > "$SERVER_DATA/$BUCKET/$DIR/obj"
    run -0 list "$BUCKET/$DIR/obj"
    assert_output 200
    run -0 cat body.xml
    assert_output data
}

@test "a GET of the bucket without the list type fails with 400" {
    run -0 list "$BUCKET"
    assert_output 400
    run -0 cat body.xml
    assert_output --partial "<Code>InvalidArgument</Code>"
}

@test "a listing of the first version fails with 400" {
    run -0 list "$BUCKET?list-type=1"
    assert_output 400
}

@test "a listing without the delimiter '/' fails with 501" {
    run -0 list "$BUCKET?list-type=2"
    assert_output 501
    run -0 cat body.xml
    assert_output --partial "<Code>NotImplemented</Code>"
}

@test "a prefix with the directory '..' fails with 400" {
    run -0 list "$BUCKET?list-type=2&delimiter=/&prefix=$DIR/../"
    assert_output 400
}

@test "a listing of a bucket without a directory fails with 404" {
    run -0 list "no-bucket?list-type=2&delimiter=/"
    assert_output 404
    run -0 cat body.xml
    assert_output --partial "<Code>NoSuchBucket</Code>"
}

# root can read a directory without the permission
@test "a listing of a directory which the server cannot read fails with 403" {
    [[ $(id -u) -ne 0 ]] || skip "root can read all directories"
    chmod 0 "$SERVER_DATA/$BUCKET/$DIR"
    run -0 list "$BUCKET?list-type=2&delimiter=/&prefix=$DIR/"
    chmod 755 "$SERVER_DATA/$BUCKET/$DIR"
    assert_output 403
    run -0 cat body.xml
    assert_output --partial "<Code>AccessDenied</Code>"
}
