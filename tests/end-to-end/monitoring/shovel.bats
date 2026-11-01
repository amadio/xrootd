#!/usr/bin/env bash

# The TCP shovel chain of xrdmoncollect. The server sends its f-stream over
# UDP to a local xrdmoncollect in shoveler mode, which relays the datagrams in
# XSHV frames over TCP, with a shared secret, to a central collector that
# listens only on TCP. The central collector must give the usual transfer
# documents, attributed to the server and not to the shoveler. An outage of
# the central collector checks the disk spool of the shoveler and its replay.
# The tests share the chain and one server.

load ../setup/server.bash
load collector.bash

BATS_TEST_TIMEOUT=180

# The central collector listens only on TCP, and has the token. -c /dev/null
# keeps the /etc/xrootd/xrdmoncollect.cfg of the host out of the test: a UDP
# port from there would make this collector bind it, and maybe fail.
setup_file() {
    require_commands openssl python3 xrdmoncollect

    printf 'shovel-e2e-secret' > "$BATS_FILE_TMPDIR/shovel.token"
    export CENTRAL_OUT="$BATS_FILE_TMPDIR/collected.ndjson"
    export DOCUMENTS=$CENTRAL_OUT
    export SPOOL_DIR="$BATS_FILE_TMPDIR/spool"
    : > "$CENTRAL_OUT"

    # the chain from its end to its start, thus the first datagram of the
    # server has the full path
    start_daemon central tcp xrdmoncollect -c /dev/null --tcp-port @PORT@ \
        --tcp-token "@$BATS_FILE_TMPDIR/shovel.token" \
        -o "$CENTRAL_OUT" --flush-secs 1 --flush-count 1

    start_daemon shoveler udp xrdmoncollect -c /dev/null -p @PORT@ \
        --shovel "127.0.0.1:$CENTRAL_PORT" --shovel-token "@$BATS_FILE_TMPDIR/shovel.token" \
        --cache-dir "$SPOOL_DIR" --flush-secs 1 --flush-count 1

    start_server shovel shovel.cfg
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    DIR=/test-n$BATS_TEST_NUMBER
    URL="$SHOVEL_URL/$DIR"
    xrdfs "$SHOVEL_URL" mkdir -p "$DIR"
    openssl rand -base64 -out ok.ref $(( 1024 * 64 ))
}

transfer() {
    xrdcp -f ok.ref "$URL/ok.ref" && xrdcp -f "$URL/ok.ref" ok.dat
}

@test "a transfer through the chain gives a document attributed to the server" {
    drive_until "\"file.path\":\"$DIR/ok.ref\"" '"xrootd.operation.state":"Successful"' -- transfer

    # The frames carry the source address of each datagram, thus the central
    # collector resolves the identity of the server as for a datagram that it
    # receives itself: a real name, not a loopback address or "unknown", and
    # the site name of the configuration.
    run -0 grep -Eq '"server.address":"[^"]+"' "$CENTRAL_OUT"
    run -1 grep -Eq '"server\.address":"(unknown|localhost[^"]*|127\.[0-9.]+|::1|::ffff:127\.[0-9.]+)"' "$CENTRAL_OUT"
    run -0 grep -Eq '"service.namespace":"shovel"' "$CENTRAL_OUT"
}

@test "the shoveler spools during an outage of the collector and replays after its restart" {
    # The chain works before the outage.
    drive_until "\"file.path\":\"$DIR/ok.ref\"" '"xrootd.operation.state":"Successful"' -- transfer

    # Stop the central collector, and transfer a file with a new name. The
    # first buffer after the stop can go into the kernel buffer of the dead
    # connection, as plain TCP has no acknowledgment of the application.
    # After the reset, each buffer goes to the spool.
    stop_pid "$(<"$BATS_FILE_TMPDIR/central/daemon.pid")"
    rm -f "$BATS_FILE_TMPDIR/central/daemon.pid"

    wait_for 30 eval 'xrdcp -f ok.ref "$URL/replay.ref" > /dev/null 2>&1; compgen -G "$SPOOL_DIR/shovel/*.frames" > /dev/null'

    # The spool keeps the order, thus these closes are on disk before the
    # collector comes back. Each waits for the fstat flush (2 s) and the
    # batch flush of the shoveler (1 s).
    for _ in 1 2; do
        xrdcp -f ok.ref "$URL/replay.ref"
        sleep 3
    done

    # After the restart, the shoveler connects again and replays the frames,
    # oldest first. The document of the file must come without a new
    # transfer.
    run_daemon central tcp "$CENTRAL_PORT" xrdmoncollect -c /dev/null --tcp-port @PORT@ \
        --tcp-token "@$BATS_FILE_TMPDIR/shovel.token" \
        -o "$CENTRAL_OUT" --flush-secs 1 --flush-count 1
    drive_until "\"file.path\":\"$DIR/replay\\.ref\"" -- true
}
