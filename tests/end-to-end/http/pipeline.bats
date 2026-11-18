#!/usr/bin/env bash

# HTTP/1.1 lets a client send several requests on a connection without a wait
# for each response (pipelining). The server must answer all of them in order,
# also when the requests arrive together in one read. The pull of XrdHttpTPC
# with more than one stream sent its requests in this way with libcurl before
# 7.62.0. The tests share the servers: a data server, and a redirector, which
# ends each request with a redirect.

load ../setup/server.bash

# a server which keeps a connection open must not block the run
# shellcheck disable=SC2034 # BATS reads it
BATS_TEST_TIMEOUT=60

setup_file() {
    require_plugin libXrdHttp
    cd "$BATS_FILE_TMPDIR"

    start_server http.cfg
    start_server rdr http.cfg pipeline-rdr.cfg

    printf '0123456789' > "$SERVER_DATA/file"
}

setup() {
    requests=
}

teardown_file() {
    stop_servers
}

# Append a request for the given method and path to $requests. The extra
# headers may use \r\n escapes. The last request of a pipeline closes the
# connection, so that the read of the responses ends.
request() {
    local method=$1 path=$2 extra=${3:-} body=${4:-} r

    printf -v r '%s %s HTTP/1.1\r\nHost: localhost\r\n%b\r\n%s' "$method" "$path" "$extra" "$body"
    requests+=$r
}

# Print what the server sends on the connection of descriptor 5, until it
# closes the connection, or sends nothing for 10 s. BATS uses the descriptors
# 3 and 4, and macOS has no timeout(1), thus read -t.
_receive() {
    local line=

    while IFS= read -r -t 10 line <&5 || [[ -n "$line" ]]; do
        printf '%s\n' "$line"
        line=
    done
}

# pipeline <port>
#
# Send $requests in one write and print the status codes received before the
# server closes the connection, or before a timeout. A status line may follow
# a response body without a newline, thus grep -o rather than a line match.
pipeline() {
    exec 5<>"/dev/tcp/localhost/$1"
    printf '%s' "$requests" >&5
    _receive | grep -ao 'HTTP/1\.1 [0-9]\{3\}' | cut -d' ' -f2 || true
    exec 5<&-
}

@test "two pipelined GET requests get two responses" {
    request GET /file 'Range: bytes=0-4\r\n'
    request GET /file 'Range: bytes=5-9\r\nConnection: close\r\n'
    run -0 pipeline "$SERVER_PORT"
    assert_output $'206\n206'
}

@test "sixteen pipelined GET requests get sixteen responses" {
    for _ in {1..15}; do
        request GET /file 'Range: bytes=0-4\r\n'
    done
    request GET /file 'Range: bytes=0-4\r\nConnection: close\r\n'
    run -0 pipeline "$SERVER_PORT"
    assert_equal "$(grep -c '^206$' <<< "$output")" 16
}

@test "a GET pipelined after a HEAD request gets a response" {
    request HEAD /file
    request GET /file 'Connection: close\r\n'
    run -0 pipeline "$SERVER_PORT"
    assert_output $'200\n200'
}

@test "a GET pipelined after a DELETE request gets a response" {
    printf 'x' > "$SERVER_DATA/removed"
    request DELETE /removed
    request GET /file 'Connection: close\r\n'
    run -0 pipeline "$SERVER_PORT"
    assert_output $'200\n200'
}

@test "a GET pipelined after an OPTIONS request gets a response" {
    request OPTIONS /file
    request GET /file 'Connection: close\r\n'
    run -0 pipeline "$SERVER_PORT"
    assert_output $'200\n200'
}

@test "a GET pipelined after a PUT gets a response" {
    request PUT /new 'Content-Length: 5\r\n' abcde
    request GET /new 'Connection: close\r\n'
    run -0 pipeline "$SERVER_PORT"
    assert_line --index 0 --regexp '^20[01]$'
    assert_line --index 1 '200'
}

@test "a redirector redirects each GET of a pipeline" {
    request GET /file
    request GET /file
    request GET /file 'Connection: close\r\n'
    run -0 pipeline "$RDR_PORT"
    assert_output $'302\n302\n302'
}

# The redirector answered the second GET with 416
@test "a redirector redirects each GET with a range of a pipeline" {
    request GET /file 'Range: bytes=0-4\r\n'
    request GET /file 'Range: bytes=5-9\r\n'
    request GET /file 'Range: bytes=0-4\r\nConnection: close\r\n'
    run -0 pipeline "$RDR_PORT"
    assert_output $'302\n302\n302'
}

@test "a redirector redirects each HEAD of a pipeline" {
    request HEAD /file
    request HEAD /file
    request HEAD /file 'Connection: close\r\n'
    run -0 pipeline "$RDR_PORT"
    assert_output $'307\n307\n307'
}
