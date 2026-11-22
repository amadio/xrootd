#!/usr/bin/env bash

# A cache (XCache) in front of an S3 origin, as XrdClS3 is used: the cache
# reads the objects of the origin with XrdClS3, in blocks, and keeps a copy.
# The servers are shared by the tests, and each test has its own objects.

load xrdcls3.bash

setup_file() {
    require_plugin libXrdPfc
    start_s3_origin origin checksum.cfg
    start_s3_server cache cache cache-checksum.cfg
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    OBJ="/$BUCKET/t${BATS_TEST_NUMBER}"
}

@test "a download through the cache returns the object of the origin" {
    echo 'Hello, World' > "$ORIGIN_DATA$OBJ"
    run -0 xrdcp -f "$CACHE_URL/$OBJ" -
    assert_output 'Hello, World'
}

# The blocks of the cache are 128 KiB (see setup/cache.cfg)
@test "a large download through the cache keeps the content of the object" {
    make_random_file "$ORIGIN_DATA$OBJ" $(( 3 * 1024 * 1024 + 7 ))
    xrdcp -f "$CACHE_URL/$OBJ" copy.dat
    run -0 cmp "$ORIGIN_DATA$OBJ" copy.dat
}

# The cache writes its copy after it sends the data
@test "the cache keeps a copy of a downloaded object" {
    make_random_file "$ORIGIN_DATA$OBJ" $(( 1024 * 1024 ))
    xrdcp -f "$CACHE_URL/$OBJ" /dev/null
    run -0 wait_for 10 cmp "$ORIGIN_DATA$OBJ" "$CACHE_DATA$OBJ"
}

@test "a stat through the cache reports the size of the object" {
    printf '0123456789' > "$ORIGIN_DATA$OBJ"
    run -0 xrdfs "$CACHE_URL" stat "$OBJ"
    assert_line --regexp '^Size: +10$'
}

@test "a read through the cache returns the bytes of the range" {
    printf '0123456789abcdefghij' > "$ORIGIN_DATA$OBJ"
    run -0 xrdcl-api read "$CACHE_URL/$OBJ" 5 5
    assert_output '56789'
}

@test "a vector read through the cache returns the bytes of each chunk" {
    printf '0123456789abcdefghij' > "$ORIGIN_DATA$OBJ"
    run -0 xrdcl-api readv "$CACHE_URL/$OBJ" 0:3 10:4
    assert_output '012abcd'
}

@test "a page read through the cache returns the bytes of the range" {
    printf '0123456789abcdefghij' > "$ORIGIN_DATA$OBJ"
    run -0 xrdcl-api pgread "$CACHE_URL/$OBJ" 2 6
    assert_line --index 0 '234567'
}

@test "a listing through the cache has the objects of the origin" {
    mkdir "$ORIGIN_DATA$OBJ"
    printf '12345' > "$ORIGIN_DATA$OBJ/obj"
    run -0 xrdfs "$CACHE_URL" ls "$OBJ"
    assert_output "$OBJ/obj"
}

@test "a stat through the cache of an object which does not exist fails" {
    run ! xrdfs "$CACHE_URL" stat "$OBJ"
    assert_output --partial '[3011]'
}

# A query without a type gets the checksum of the default type of the origin,
# with the name of the default type of the cache, thus the test names the type
@test "a checksum query through the cache returns the checksum of the origin" {
    printf '0123456789abcdefghij' > "$ORIGIN_DATA$OBJ"
    run -0 xrdfs "$ORIGIN_URL" query checksum "$OBJ?cks.type=adler32"
    local expected=$output
    run -0 xrdfs "$CACHE_URL" query checksum "$OBJ?cks.type=adler32"
    assert_output "$expected"
}
