#!/usr/bin/env bash

# The unix security protocol, which is the only protocol of the default
# server. The client sends the names of its user and group, and the server
# asks for no proof. Each test starts its own server, as a fault of a
# request can stop it.

load ../setup/server.bash

setup_file() {
    require_commands python3
}

setup() {
    start_server
}

teardown() {
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
