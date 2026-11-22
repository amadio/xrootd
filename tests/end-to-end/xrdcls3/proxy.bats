#!/usr/bin/env bash

# A proxy without a cache in front of an S3 origin: the proxy reads and
# writes the objects of the origin with XrdClS3. The clients of the proxy use
# root:// URLs, or https:// URLs with curl. The servers are shared by the
# tests, and each test has its own objects.

load xrdcls3.bash

setup_file() {
    start_s3_origin origin
    start_s3_server proxy proxy http.cfg tls.cfg
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    OBJ="/$BUCKET/t${BATS_TEST_NUMBER}"
}

@test "a download through the proxy returns the object of the origin" {
    echo 'Hello, World' > "$ORIGIN_DATA$OBJ"
    run -0 xrdcp -f "$PROXY_URL/$OBJ" -
    assert_output 'Hello, World'
}

@test "an HTTPS download from the proxy returns the object of the origin" {
    require_commands curl
    echo 'Hello, World' > "$ORIGIN_DATA$OBJ"
    run -0 curl --silent --fail --cacert "$CREDENTIALS/ca.pem" "$PROXY_HTTPS$OBJ"
    assert_output 'Hello, World'
}

@test "an HTTPS download from the proxy of a missing object fails with 404" {
    require_commands curl
    run -0 curl --silent -o /dev/null --write-out '%{http_code}' \
        --cacert "$CREDENTIALS/ca.pem" "$PROXY_HTTPS$OBJ"
    assert_output 404
}

@test "a large download through the proxy keeps the content of the object" {
    make_random_file "$ORIGIN_DATA$OBJ" $(( 5 * 1024 * 1024 + 7 ))
    xrdcp -f "$PROXY_URL/$OBJ" copy.dat
    run -0 cmp "$ORIGIN_DATA$OBJ" copy.dat
}

@test "a stat through the proxy reports the size of the object" {
    printf '0123456789' > "$ORIGIN_DATA$OBJ"
    run -0 xrdfs "$PROXY_URL" stat "$OBJ"
    assert_line --regexp '^Size: +10$'
}

@test "a read through the proxy returns the bytes of the range" {
    printf '0123456789abcdefghij' > "$ORIGIN_DATA$OBJ"
    run -0 xrdcl-api read "$PROXY_URL/$OBJ" 5 5
    assert_output '56789'
}

@test "a vector read through the proxy returns the bytes of each chunk" {
    printf '0123456789abcdefghij' > "$ORIGIN_DATA$OBJ"
    run -0 xrdcl-api readv "$PROXY_URL/$OBJ" 0:3 10:4
    assert_output '012abcd'
}

@test "a page read through the proxy returns the bytes of the range" {
    printf '0123456789abcdefghij' > "$ORIGIN_DATA$OBJ"
    run -0 xrdcl-api pgread "$PROXY_URL/$OBJ" 2 6
    assert_line --index 0 '234567'
}

# XrdClS3 and XrdClHttp have no PgWrite, thus an upload through the proxy
# fails with kXR_pgwrite, which xrdcp uses by default. The uploads use
# kXR_write.
@test "an upload through the proxy writes the object to the origin" {
    echo 'new data' > local.dat
    XRD_CPUSEPGWRTRD=0 run -0 xrdcp local.dat "$PROXY_URL/$OBJ"
    run -0 cmp local.dat "$ORIGIN_DATA$OBJ"
}

@test "a large upload through the proxy keeps the content of the object" {
    make_random_file local.dat $(( 5 * 1024 * 1024 + 7 ))
    XRD_CPUSEPGWRTRD=0 run -0 xrdcp local.dat "$PROXY_URL/$OBJ"
    run -0 cmp local.dat "$ORIGIN_DATA$OBJ"
}

@test "a removal through the proxy removes the object of the origin" {
    echo 'old data' > "$ORIGIN_DATA$OBJ"
    run -0 xrdfs "$PROXY_URL" rm "$OBJ"
    assert [ ! -e "$ORIGIN_DATA$OBJ" ]
}

# S3 has no directories. XrdClS3 writes an empty object in the directory, the
# sentinel, and the origin makes the directory with it.
@test "a mkdir through the proxy writes the sentinel of the directory" {
    run -0 xrdfs "$PROXY_URL" mkdir "$OBJ"
    assert [ -f "$ORIGIN_DATA$OBJ/.xrdcls3.dirsentinel" ]
}

# The removal of the sentinel removes the directory of S3, which exists only
# through its keys. The origin of the tests keeps the empty directory.
@test "a rmdir through the proxy removes the sentinel of the directory" {
    xrdfs "$PROXY_URL" mkdir "$OBJ"
    run -0 xrdfs "$PROXY_URL" rmdir "$OBJ"
    assert [ ! -e "$ORIGIN_DATA$OBJ/.xrdcls3.dirsentinel" ]
}
