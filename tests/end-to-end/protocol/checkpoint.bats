#!/usr/bin/env bash

# Checkpoint requests of the XRootD protocol which the clients of XRootD do
# not send, from the raw client of the setup. The server has no security.
# Each test starts its own server, as a fault of the request can stop it.

load ../setup/server.bash

setup_file() {
    require_commands python3
}

setup() {
    start_server nosec server-nosec
}

teardown() {
    stop_servers
}

# A checkpoint request of kXR_ckpXeq carries a request as its data. Without
# data, the server read the request through a null pointer.
@test "a checkpoint request without the request that it carries gets kXR_ArgInvalid" {
    run -0 python3 "$SETUP_DIR/raw-client.py" localhost "$NOSEC_PORT" chkpoint-xeq
    assert_output --partial 'error 3000'
}

@test "a server stays up after a checkpoint request without the request that it carries" {
    python3 "$SETUP_DIR/raw-client.py" localhost "$NOSEC_PORT" chkpoint-xeq
    run -0 xrdfs "$NOSEC_URL" query config version
}
