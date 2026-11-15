#!/usr/bin/env bash

# The unix security protocol, which is the only protocol of the default
# server. The client sends the names of its user and group, and the server
# asks for no proof.

load ../setup/server.bash

setup_file() {
    require_commands python3
    start_server
}

teardown_file() {
    stop_servers
}

# The login request of the raw client has no body. Before the check of the
# argument, the server then read the credentials through a null pointer.
@test "an authentication request without credentials gets kXR_ArgMissing" {
    run -0 python3 "$SETUP_DIR/raw-client.py" localhost "$SERVER_PORT" auth-empty unix
    assert_output --partial 'error 3001'
}

@test "the server stays up after an authentication request without credentials" {
    python3 "$SETUP_DIR/raw-client.py" localhost "$SERVER_PORT" auth-empty unix
    run -0 xrdfs "$SERVER_URL" query config version
}

# The server checked the authentication only after the requests with a file
# handle. A checkpoint request without data then read the request that it
# carries through a null pointer.
@test "a checkpoint request before authentication gets kXR_InvalidRequest" {
    run -0 python3 "$SETUP_DIR/raw-client.py" localhost "$SERVER_PORT" chkpoint-xeq
    assert_output --partial 'error 3006'
}

@test "the server stays up after a checkpoint request before authentication" {
    python3 "$SETUP_DIR/raw-client.py" localhost "$SERVER_PORT" chkpoint-xeq
    run -0 xrdfs "$SERVER_URL" query config version
}
