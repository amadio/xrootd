#!/usr/bin/env bash

# The connections which the destination of a pull copy holds while the data
# flows (issue #2995). The servers of each test count the connections of a
# single copy only, thus each test starts its own servers. The source sends
# the file of 256 MiB at 4 MB/s, thus a copy lasts about 64 s. A test checks
# the count while the copy runs, as no connection is left after its end, and
# stops the copy when the check passes.

load ../../setup/server.bash

export XRD_PLUGINCONFDIR="$SETUP_DIR/client-plugins"
export no_proxy=localhost NO_PROXY=localhost

# shellcheck disable=SC2034 # BATS reads it
BATS_TEST_TIMEOUT=120

setup() {
	require_plugin libXrdHttpTPC
	cd "$BATS_TEST_TMPDIR"

	start_server src http.cfg http-tpc.cfg connections-src.cfg mirage-xattr.cfg trace-http.cfg
	start_server dst http.cfg http-tpc.cfg trace-http.cfg
	start_server rdr http.cfg connections-rdr.cfg trace-http.cfg

	head -c 1 /dev/zero | xrdcp - "$SRC_HTTP//file_src"
	xrdfs "$SRC_URL" truncate /file_src $(( 256 << 20 ))
}

teardown() {
	if [[ -s "$BATS_TEST_TMPDIR/copy.pid" ]]; then
		stop_pid "$(<"$BATS_TEST_TMPDIR/copy.pid")"
	fi
	stop_servers
}

# start_pull <source> [streams]
#
# Start a pull copy, the destination being the only client of the source and
# the redirector, and wait until its first GET reaches the source
start_pull() {
	local source=$1 streams=${2:-1}

	# without fd 3, bats does not wait for the copy to end
	curl -s -X COPY -H "Source: $source" -H "X-Number-Of-Streams: $streams" \
		"$DST_HTTP/file_dst" >/dev/null 2>&1 3>&- &
	echo $! > "$BATS_TEST_TMPDIR/copy.pid"

	wait_for 60 grep -qsF 'got hdr line: GET /file_src' "$SRC_LOG"
}

# during_copy <command...>
#
# Wait up to 30 s until the command succeeds while the copy still runs. A
# loaded host, or valgrind, needs more than 10 s.
during_copy() {
	wait_for 30 _while_copying "$(<"$BATS_TEST_TMPDIR/copy.pid")" "$@"
}

_while_copying() {
	local pid=$1
	shift
	is_running "$pid" && "$@"
}

@test "pull via a redirector holds no idle connection to the redirector" {
	start_pull "$RDR_HTTP/file_src"
	run -0 during_copy tcp_connections_at_most "$RDR_PORT" 0
}

@test "pull via a redirector holds one connection to the source" {
	start_pull "$RDR_HTTP/file_src"
	run -0 during_copy tcp_connections_are "$SRC_PORT" 1
}

# libcurl before 8.7.0 kept the connection of the HEAD open while the GET
# ran; a newer libcurl passes this test also without the fix
@test "pull from the source holds a single connection" {
	start_pull "$SRC_HTTP/file_src"
	run -0 during_copy tcp_connections_are "$SRC_PORT" 1
}

# The streams reuse their connections, thus each one keeps its connection to
# the redirector; the connections of the HEAD are closed
@test "pull with 4 streams via a redirector holds at most 4 connections to the redirector" {
	start_pull "$RDR_HTTP/file_src" 4
	run -0 during_copy tcp_connections_at_most "$RDR_PORT" 4
}

@test "pull with 4 streams holds at most 4 connections to the source" {
	start_pull "$SRC_HTTP/file_src" 4
	run -0 during_copy tcp_connections_at_most "$SRC_PORT" 4
}
