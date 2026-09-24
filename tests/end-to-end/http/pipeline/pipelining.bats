#!/usr/bin/env bash

# HTTP/1.1 lets a client send several requests on a connection without a wait
# for each response (pipelining). The server must answer all of them in order,
# also when the requests arrive together in one read.

bats_require_minimum_version 1.5.0

bats_load_library 'bats-support'
bats_load_library 'bats-assert'

load ../../helper/common.bash

PORT=9882

setup() {
	requests=
	cd $BATS_TEST_TMPDIR

	PORT=$PORT launch_xrootd pipeline.cfg pipeline

	printf '0123456789' > pipeline/file
}

teardown() {
	kill_pid_files
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
# server closes the connection, or before a timeout. A status line may follow
# a response body without a newline, thus grep -o rather than a line match.
pipeline() {
	exec 3<>/dev/tcp/localhost/$PORT
	printf '%s' "$requests" >&3
	timeout 10 cat <&3 | grep -ao 'HTTP/1\.1 [0-9]\{3\}' | cut -d' ' -f2 || true
	exec 3<&-
}

@test "two pipelined GET requests get two responses" {
	request GET /file 'Range: bytes=0-4\r\n'
	request GET /file 'Range: bytes=5-9\r\nConnection: close\r\n'
	run -0 pipeline
	assert_output $'206\n206'
}

@test "sixteen pipelined GET requests get sixteen responses" {
	for i in {1..15}; do
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
	printf 'x' > pipeline/removed
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
