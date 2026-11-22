#!/usr/bin/env bash

# Errors of an S3 origin, which reach the clients of a proxy and of a cache
# through XrdClS3. The origin has the Mirage OSS, which returns the errors of
# the extended attributes of a file. The servers are shared by the tests, and
# each test has its own object, of 100000 bytes of the pattern "abcde".

load xrdcls3.bash

setup_file() {
    require_plugin libXrdOssMirage
    require_plugin libXrdPfc
    start_s3_origin origin mirage.cfg
    start_s3_server proxy proxy
    start_s3_server cache cache
}

teardown_file() {
    stop_servers
}

# Mirage has no directories, thus the object is a file of its own at the path
setup() {
    cd "$BATS_TEST_TMPDIR"
    OBJ="/$BUCKET/t${BATS_TEST_NUMBER}"
    printf 'seed' | xrdcp -s - "$ORIGIN_URL/$OBJ"
    xrdfs "$ORIGIN_URL" truncate "$OBJ" 100000
    xrdfs "$ORIGIN_URL" xattr "$OBJ" set pattern=abcde
}

# set_error <name> <value>: set an error attribute of the object
set_error() {
    xrdfs "$ORIGIN_URL" xattr "$OBJ" set "$1=$2"
}

@test "a download through the proxy returns the pattern of the object" {
    xrdcp -f "$PROXY_URL/$OBJ" copy.dat
    run -0 wc -c < copy.dat
    assert_output --regexp '^ *100000 *$'
    run -0 head -c 10 copy.dat
    assert_output 'abcdeabcde'
}

@test "an open error of the origin fails a download through the proxy" {
    set_error open.return_code 13
    run ! xrdcp -f "$PROXY_URL/$OBJ" copy.dat
    assert_output --partial '[3010]'
}

@test "a read error of the origin fails a download through the proxy" {
    set_error read.return_code 5
    set_error read.return_position 50000
    run ! xrdcp -f "$PROXY_URL/$OBJ" copy.dat
}

@test "a write error of the origin fails an upload through the proxy" {
    set_error write.return_code 5
    make_random_file local.dat 1000
    XRD_CPUSEPGWRTRD=0 run ! xrdcp -f local.dat "$PROXY_URL/$OBJ"
}

@test "an open error of the origin fails a download through the cache" {
    set_error open.return_code 13
    run ! xrdcp -f "$CACHE_URL/$OBJ" copy.dat
    assert_output --partial '[3010]'
}

@test "a read error of the origin fails a download through the cache" {
    set_error read.return_code 5
    set_error read.return_position 50000
    run ! xrdcp -f "$CACHE_URL/$OBJ" copy.dat
}
