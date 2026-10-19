#!/usr/bin/env bash

# Tests of the common server setup itself (see server.bash).

load server.bash

setup_file() {
    start_server shared
    start_server other
}

teardown_file() {
    stop_servers
}

# A test which fails before it stops its stand-in process must not leave it,
# else BATS waits for its output
teardown() {
    [[ -z "${STANDIN:-}" ]] || kill "$STANDIN" 2>/dev/null || true
    stop_servers
}

@test "a server shared by the test file answers on its URL" {
    run -0 xrdfs "$SHARED_URL" query config version
}

@test "a server answers on its IPv4 URL" {
    run -0 xrdfs "$SHARED_URL4" query config version
}

@test "a server answers on its IPv6 URL" {
    require_ipv6
    run -0 xrdfs "$SHARED_URL6" query config version
}

@test "two servers of a test file use different ports" {
    assert_not_equal "$SHARED_PORT" "$OTHER_PORT"
}

@test "the data directory of a server is its local root" {
    echo 'seed' > "$SHARED_DATA/data-dir"
    run -0 xrdcp "$SHARED_URL//data-dir" -
    assert_output 'seed'
}

@test "the server log has the configuration of the role" {
    start_server mine
    run -0 grep -F "oss.localroot $BATS_TEST_TMPDIR/mine/data" "$MINE_LOG"
}

@test "each test starts with an empty log of a server of the test file" {
    run ! test -s "$SHARED_LOG"
}

@test "a server of a test runs below the test directory" {
    start_server mine
    assert_equal "$MINE_DATA" "$BATS_TEST_TMPDIR/mine/data"
}

@test "a server of a test can use the name of a server of the test file" {
    start_server shared
    run -0 xrdfs "$SHARED_URL" query config version
}

@test "stop_servers stops the servers of the test" {
    local pid
    start_server mine
    pid=$(<"$BATS_TEST_TMPDIR/mine/xrootd.pid")
    stop_servers
    run ! is_running "$pid"
}

@test "stop_servers does not stop the servers of the test file" {
    start_server mine
    stop_servers
    run -0 server_is_up "$OTHER_URL"
}

@test "stop_server stops only the given server" {
    start_server mine
    start_server yours
    stop_server mine
    run -0 server_is_up "$YOURS_URL"
}

@test "stop_server stops the process of the server" {
    local pid
    start_server mine
    pid=$(<"$BATS_TEST_TMPDIR/mine/xrootd.pid")
    stop_server mine
    run ! is_running "$pid"
}

@test "no server answers on NO_SERVER_PORT" {
    run ! server_is_up "root://localhost:$NO_SERVER_PORT"
}

@test "the tpc snippet runs the xrdcp of the build" {
    start_server mine tpc.cfg
    run -0 grep -F "ofs.tpc pgm $(dirname "$(command -v xrootd)")/xrdcp --server" "$MINE_LOG"
}

@test "a snippet name selects a snippet of the setup" {
    require_plugin libXrdHttp
    start_server mine http.cfg
    run -0 grep -F 'xrd.protocol XrdHttp libXrdHttp.so' "$MINE_LOG"
}

@test "a snippet in the test and in the setup directory is an error" {
    local BATS_TEST_DIRNAME=$BATS_TEST_TMPDIR
    cp "$SETUP_DIR/http.cfg" "$BATS_TEST_TMPDIR/http.cfg"
    run ! start_server mine http.cfg
    assert_output --partial "snippet 'http.cfg' is in the test and in the setup directory"
}

@test "the http snippet serves http URLs on the port of the server" {
    require_plugin libXrdHttp
    start_server mine http.cfg
    echo 'seed' > "$MINE_DATA/file"
    run -0 curl -sf "$MINE_HTTP/file"
    assert_output 'seed'
}

@test "the tls snippet serves https URLs with the shared host certificate" {
    require_plugin libXrdHttp
    start_server mine http.cfg tls.cfg
    echo 'seed' > "$MINE_DATA/file"
    run -0 curl -sf --cacert "$CREDENTIALS/ca.pem" "$MINE_HTTPS/file"
    assert_output 'seed'
}

@test "server_log of a server of the test file has the requests of the test" {
    xrdfs "$SHARED_URL" stat "/t${BATS_TEST_NUMBER}" || true
    run -0 grep -F "/t${BATS_TEST_NUMBER}" <(server_log shared)
}

# The test just before this one asked for /t<its number>.
@test "server_log of a server of the test file has no request of an earlier test" {
    xrdfs "$SHARED_URL" stat /a-path-of-this-test || true
    run ! grep -F "/t$(( BATS_TEST_NUMBER - 1 ))" <(server_log shared)
}

@test "server_log of a server of the test has its whole log" {
    start_server mine
    run -0 grep -F 'initialization completed' <(server_log mine)
}

@test "restart_server keeps the port" {
    start_server mine
    restart_server mine
    run -0 server_is_up "$MINE_URL"
}

@test "restart_server keeps the data" {
    start_server mine
    echo 'seed' | xrdcp - "$MINE_URL//kept"
    restart_server mine
    run -0 xrdcp "$MINE_URL//kept" -
    assert_output 'seed'
}

@test "restart_server fails when the server cannot start again" {
    start_server mine
    cat "$BATS_TEST_DIRNAME/broken.cfg" >> "$BATS_TEST_TMPDIR/mine/mine.cfg"
    run ! restart_server mine
}

@test "a restart on a port in use fails" {
    start_server mine
    echo "$SHARED_PORT" > "$BATS_TEST_TMPDIR/mine/port"
    run ! restart_server mine
}

@test "start_server fails when the configuration is not valid" {
    run ! start_server broken broken.cfg
    assert_output --partial 'initialization failed'
}

@test "start_server leaves no process when the server fails to start" {
    run ! start_server broken broken.cfg
    run ! pgrep -f -- "$BATS_TEST_TMPDIR/broken/"
}

@test "start_server refuses a second server with the same name" {
    start_server mine
    run ! start_server mine
}

@test "start_server without arguments starts the server named server" {
    start_server
    run -0 server_is_up "$SERVER_URL"
}

@test "start_server takes a snippet without the name and the role" {
    start_server tpc.cfg
    run -0 grep -F "ofs.tpc pgm" "$SERVER_LOG"
}

@test "start_server refuses a third word after the name and the role" {
    run ! start_server mine server-nosec other
    assert_output --partial "unexpected argument 'other'"
}

@test "a server runs with its name as instance name" {
    start_server named
    run -0 grep -F "xrootd named@" "$NAMED_LOG"
}

# With the same seed, start_server first tries the port of the server busy
@test "start_server takes another port when its port is in use" {
    local port
    RANDOM=42
    port=$(( 21000 + RANDOM % 11000 ))
    start_server busy
    echo "$port" > "$BATS_TEST_TMPDIR/busy/port"
    restart_server busy
    RANDOM=42
    start_server mine
    assert_not_equal "$MINE_PORT" "$port"
}

@test "a long run directory moves the admin path out of it" {
    start_server "long-$(printf 'x%.0s' {1..30})"
    run -0 test -L "$BATS_TEST_TMPDIR/long-$(printf 'x%.0s' {1..30})/admin"
}

@test "stop_servers removes the admin path outside of the run directory" {
    local name admin
    name="long-$(printf 'x%.0s' {1..30})"
    start_server "$name"
    admin=$(readlink "$BATS_TEST_TMPDIR/$name/admin")
    stop_servers
    run ! test -e "$admin"
}

@test "each test has its own client log" {
    xrdfs "$SHARED_URL" query config version
    run -0 test -s "$BATS_TEST_TMPDIR/client.log"
}

@test "the client authenticates with unix" {
    xrdfs "$SHARED_URL" query config version
    run -0 grep -F 'Authenticated with unix' "$XRD_LOGFILE"
}

@test "the shared credentials are read-only" {
    # shellcheck disable=SC2016 # the inner bash expands the variable
    run ! bash -c 'echo >> "$CREDENTIALS/ca.pem"'
}

@test "the shared host certificate names localhost and both loopback addresses" {
    run -0 openssl x509 -in "$CREDENTIALS/host.pem" -noout -ext subjectAltName
    assert_output --regexp 'DNS:localhost.*IP Address:127\.0\.0\.1.*IP Address:0:0:0:0:0:0:0:1'
}

@test "the client logs at the Debug level or above" {
    assert_regex "$XRD_LOGLEVEL" '^(Debug|Dump)$'
}

# Run the reaper for the run directory of the test; the processes <pids>
# stand for BATS
reap() {
    "$SETUP_DIR/reaper.sh" "$1" "$BATS_TEST_TMPDIR" "${2:-}" < /dev/null > /dev/null 2>&1
}

_stopped() {
    ! is_running "$(<"$BATS_TEST_TMPDIR/$1/xrootd.pid")"
}

@test "the reaper stops a server when BATS exits without the teardown" {
    start_server reaped
    sleep 600 &
    local bats=$!
    reap "$bats" &
    kill "$bats"
    run -0 wait_for 10 _stopped reaped
}

# After a normal exit, BATS removes the run directory with the pid files
@test "the reaper stops a server without its pid file" {
    start_server reaped
    local pid
    pid=$(<"$BATS_TEST_TMPDIR/reaped/xrootd.pid")
    rm "$BATS_TEST_TMPDIR/reaped/xrootd.pid"
    reap "$(sh -c 'echo $$')"
    run ! is_running "$pid"
}

@test "the reaper waits while BATS runs" {
    start_server reaped
    sleep 600 &
    STANDIN=$!
    reap "$STANDIN" &
    sleep 2
    run -0 is_running "$(<"$BATS_TEST_TMPDIR/reaped/xrootd.pid")"
}

@test "the reaper waits while one of the processes of BATS runs" {
    start_server reaped
    sleep 600 &
    STANDIN=$!
    reap "$(sh -c 'echo $$') $STANDIN" &
    sleep 2
    run -0 is_running "$(<"$BATS_TEST_TMPDIR/reaped/xrootd.pid")"
}

@test "the reaper does not stop a process of another run with a pid file in the run" {
    sleep 600 &
    STANDIN=$!
    mkdir "$BATS_TEST_TMPDIR/other"
    echo "$STANDIN" > "$BATS_TEST_TMPDIR/other/other.pid"
    reap "$(sh -c 'echo $$')"
    run -0 is_running "$STANDIN"
}

@test "the reaper removes the run directory when BATS cleans up" {
    mkdir "$BATS_TEST_TMPDIR/run"
    "$SETUP_DIR/reaper.sh" "$(sh -c 'echo $$')" "$BATS_TEST_TMPDIR/run" 1
    run ! test -e "$BATS_TEST_TMPDIR/run"
}

@test "the reaper keeps the run directory with --no-tempdir-cleanup" {
    mkdir "$BATS_TEST_TMPDIR/run"
    "$SETUP_DIR/reaper.sh" "$(sh -c 'echo $$')" "$BATS_TEST_TMPDIR/run" ''
    run -0 test -d "$BATS_TEST_TMPDIR/run"
}

@test "restart_server in a test restarts a server of the test file" {
    restart_server other
    run -0 server_is_up "$OTHER_URL"
}
