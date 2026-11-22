#!/usr/bin/env bash

# Listings and stat through a proxy in front of an S3 origin. XrdClS3 lists a
# directory with the S3 request ListObjectsV2, and also uses it to find a
# directory, which S3 does not have. The handler XrdHttpS3 of the origin
# answers the request. The servers are shared by the tests, and each test has
# its own directory.

load xrdcls3.bash

setup_file() {
    start_s3_origin origin
    start_s3_server proxy proxy
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    DIR="/$BUCKET/t${BATS_TEST_NUMBER}"
    mkdir "$ORIGIN_DATA$DIR"
}

@test "a listing of the bucket has its directories" {
    run -0 xrdfs "$PROXY_URL" ls "/$BUCKET"
    assert_line "$DIR"
}

@test "a listing of a directory has its objects and its directories" {
    printf '12345' > "$ORIGIN_DATA$DIR/obj"
    mkdir "$ORIGIN_DATA$DIR/sub"
    printf 'x' > "$ORIGIN_DATA$DIR/sub/inner"
    run -0 xrdfs "$PROXY_URL" ls -l "$DIR"
    assert_equal "${#lines[@]}" 2
    assert_line --index 0 --regexp "^-.* 5 .* $DIR/obj\$"
    assert_line --index 1 --regexp "^d.* $DIR/sub\$"
}

# S3 sends at most 1000 keys in a response, and XrdClS3 asks for the next
# page with the token of the response
@test "a listing of more than 1000 objects has all the objects" {
    (cd "$ORIGIN_DATA$DIR" && for i in $(seq -w 1 1203); do : > "o$i"; done)
    run -0 xrdfs "$PROXY_URL" ls "$DIR"
    assert_equal "${#lines[@]}" 1203
    assert_line --index 0 "$DIR/o0001"
    assert_line --index 1202 "$DIR/o1203"
}

# The handler encodes the keys for the URL, and XrdClS3 decodes them
@test "a listing keeps the characters of the names" {
    : > "$ORIGIN_DATA$DIR/a b&c"
    : > "$ORIGIN_DATA$DIR/x+y=z%"
    run -0 xrdfs "$PROXY_URL" ls "$DIR"
    assert_line "$DIR/a b&c"
    assert_line "$DIR/x+y=z%"
}

@test "a listing of a directory which does not exist fails" {
    run ! xrdfs "$PROXY_URL" ls "$DIR/missing"
    assert_output --partial '[3011]'
}

# XrdClS3 checks a key which is not an object with a listing, because a
# directory of S3 is only the start of its keys
@test "a stat of an object which does not exist fails with not found" {
    run ! xrdfs "$PROXY_URL" stat "$DIR/missing"
    assert_output --partial '[3011]'
}

@test "a stat of the bucket reports a directory" {
    run -0 xrdfs "$PROXY_URL" stat "/$BUCKET"
    assert_line --regexp '^Flags: .*IsDir'
}

@test "a directory of mkdir exists, and its parent lists it" {
    run ! xrdfs "$PROXY_URL" stat "$DIR/new"
    assert_output --partial '[3011]'
    run -0 xrdfs "$PROXY_URL" mkdir "$DIR/new"
    run -0 xrdfs "$PROXY_URL" stat "$DIR/new"
    assert_line --regexp '^Flags: .*IsDir'
    run -0 xrdfs "$PROXY_URL" ls -l "$DIR"
    assert_output --regexp "^d.* $DIR/new\$"
}

# The sentinel of mkdir marks the directory, and is not an entry of it
@test "a listing of a directory of mkdir does not have the sentinel" {
    run -0 xrdfs "$PROXY_URL" mkdir "$DIR/new"
    run -0 xrdfs "$PROXY_URL" ls "$DIR/new"
    assert_output ''
    printf 'x' > "$ORIGIN_DATA$DIR/new/obj"
    run -0 xrdfs "$PROXY_URL" ls "$DIR/new"
    assert_output "$DIR/new/obj"
}
