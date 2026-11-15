#!/usr/bin/env bash

# Logins to a server without security and to a server which requires unix
# authentication. The tests share the servers.

load ../setup/server.bash

setup_file() {
    start_server nosec server-nosec
    start_server unix
}

teardown_file() {
    stop_servers
}

# A test which needs the start of a server has its own server
teardown() {
    stop_servers
}

@test "a server without security accepts a client which does not authenticate" {
    run -0 xrdfs "$NOSEC_URL" query config version
}

@test "a server without security logs a login without a user name" {
    xrdfs "$NOSEC_URL" query config version
    run -0 grep -E 'XrootdXeq: .* login$' <(server_log nosec)
}

@test "a server without security warns that it has no security library" {
    start_server nosec server-nosec
    run -0 grep -F "Config warning: 'xrootd.seclib' not specified" "$NOSEC_LOG"
}

@test "a server with unix authentication logs the login as the user" {
    xrdfs "$UNIX_URL" query config version
    run -0 grep -E "XrootdXeq: .* login as $(id -un)\$" <(server_log unix)
}

@test "the client authenticates with unix to a server which requires it" {
    xrdfs "$UNIX_URL" query config version
    run -0 grep -F 'Authenticated with unix' "$XRD_LOGFILE"
}

# A checkpoint request of kXR_ckpXeq carries a request as its data. Without
# data, the server read the request through a null pointer.
@test "a checkpoint request without the request that it carries gets kXR_ArgInvalid" {
    require_commands python3
    run -0 python3 "$SETUP_DIR/raw-client.py" localhost "$NOSEC_PORT" chkpoint-xeq
    assert_output --partial 'error 3000'
}

@test "a server without security stays up after a checkpoint request without data" {
    require_commands python3
    python3 "$SETUP_DIR/raw-client.py" localhost "$NOSEC_PORT" chkpoint-xeq
    run -0 xrdfs "$NOSEC_URL" query config version
}
