#!/usr/bin/env bash

# HTTP/1.1 lets a client send several requests on a connection without a wait
# for each response (pipelining). The server must answer all of them in order,
# also when the requests arrive together in one read.

load ../setup/server.bash

setup_file() {
	require_plugin libXrdHttp
	require_commands python3
}

setup() {
	requests=
	start_server http.cfg
	printf '0123456789' > "$SERVER_DATA/file"
}

teardown() {
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

# Send $requests in one write and print the status codes received before the
# server closes the connection, or before a timeout of 10 s. A status line may
# follow a response body without a newline, thus a search in the whole output
# rather than a line match.
pipeline() {
	printf '%s' "$requests" | python3 -c '
import re, socket, sys
response = b""
with socket.create_connection(("localhost", int(sys.argv[1])), timeout=10) as s:
    s.sendall(sys.stdin.buffer.read())
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            response += chunk
    except socket.timeout:
        pass
for code in re.findall(rb"HTTP/1\.1 ([0-9]{3})", response):
    print(code.decode())
' "$SERVER_PORT"
}

@test "two pipelined GET requests get two responses" {
	request GET /file 'Range: bytes=0-4\r\n'
	request GET /file 'Range: bytes=5-9\r\nConnection: close\r\n'
	run -0 pipeline
	assert_output $'206\n206'
}

@test "sixteen pipelined GET requests get sixteen responses" {
	for _ in {1..15}; do
		request GET /file 'Range: bytes=0-4\r\n'
	done
	request GET /file 'Range: bytes=0-4\r\nConnection: close\r\n'
	run -0 pipeline
	assert_equal "$(grep -c '^206$' <<< "$output")" 16
}

@test "a GET pipelined after a HEAD request gets a response" {
	request HEAD /file
	request GET /file 'Connection: close\r\n'
	run -0 pipeline
	assert_output $'200\n200'
}

@test "a GET pipelined after a DELETE request gets a response" {
	printf 'x' > "$SERVER_DATA/removed"
	request DELETE /removed
	request GET /file 'Connection: close\r\n'
	run -0 pipeline
	assert_output $'200\n200'
}

@test "a GET pipelined after an OPTIONS request gets a response" {
	request OPTIONS /file
	request GET /file 'Connection: close\r\n'
	run -0 pipeline
	assert_output $'200\n200'
}

@test "a GET pipelined after a PUT gets a response" {
	request PUT /new 'Content-Length: 5\r\n' abcde
	request GET /new 'Connection: close\r\n'
	run -0 pipeline
	assert_line --index 0 --regexp '^20[01]$'
	assert_line --index 1 '200'
}
