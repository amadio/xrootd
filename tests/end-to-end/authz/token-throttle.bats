#!/usr/bin/env bash

# The throttle counts the open files of each user, and a token selects the
# user with its subject. A token module sets the subject during the
# authorization of the open, thus the throttle must authorize the open before
# it reads the user. Otherwise all the token clients count as the user of the
# connection. The throttle must also check the limit before the wrapped open,
# which creates or truncates the file.
#
# The limit of the server is one open file for each user.
# The trace of the throttle writes the user of each open into the server log.
# The subject of the test tokens is "test".

load token-server.bash

# hold_file and release_file wait for xrdcp
# shellcheck disable=SC2034 # BATS reads it
BATS_TEST_TIMEOUT=60

setup_file() {
    start_token_server token-throttle.cfg

    make_token storage.modify:/protected > "$BATS_FILE_TMPDIR/write.token"
    chmod 0600 "$BATS_FILE_TMPDIR/write.token"
}

teardown_file() {
    stop_servers
}

# The throttle logs the number of open files of the user at each open. It
# logs "opened their first file" only for a user without a count, thus that
# line depends on the earlier tests.
@test "an open with a token counts for the subject of the token" {
    TOKEN=$(<"$BATS_FILE_TMPDIR/read.token")
    run -0 xrdfs "$SERVER_URL" cat "/protected/hello?authz=$TOKEN"
    run -0 grep -F "User test has 1 open files" <(server_log server)
}

# hold_file <path>: keep the file of the server open for writing, until
# release_file. xrdcp opens the file, then waits for the end of the FIFO.
hold_file() {
    mkfifo "$BATS_TEST_TMPDIR/hold.fifo"
    xrdcp -f - "$SERVER_URL/$1?authz=$WRITE" < "$BATS_TEST_TMPDIR/hold.fifo" 3>&- &
    HOLDER=$!
    exec 9> "$BATS_TEST_TMPDIR/hold.fifo"
    wait_for 10 test -e "$SERVER_DATA$1"
}

# Wait only for the xrdcp: with BATS_TEST_TIMEOUT, BATS runs a job of its own
release_file() {
    exec 9>&-
    wait "$HOLDER"
}

@test "an open over the limit of the user fails" {
    WRITE=$(<"$BATS_FILE_TMPDIR/write.token")
    echo 'new content' > "$BATS_TEST_TMPDIR/new.txt"
    hold_file "/protected/hold$BATS_TEST_NUMBER"
    run xrdcp -f "$BATS_TEST_TMPDIR/new.txt" "$SERVER_URL//protected/new$BATS_TEST_NUMBER?authz=$WRITE"
    release_file
    assert_failure
    assert_output --partial 'User test has hit the limit of 1 open files'
}

@test "an open over the limit of the user creates no file" {
    WRITE=$(<"$BATS_FILE_TMPDIR/write.token")
    echo 'new content' > "$BATS_TEST_TMPDIR/new.txt"
    hold_file "/protected/hold$BATS_TEST_NUMBER"
    run xrdcp -f "$BATS_TEST_TMPDIR/new.txt" "$SERVER_URL//protected/new$BATS_TEST_NUMBER?authz=$WRITE"
    release_file
    run ! test -e "$SERVER_DATA/protected/new$BATS_TEST_NUMBER"
}

@test "an open over the limit of the user does not truncate the file" {
    WRITE=$(<"$BATS_FILE_TMPDIR/write.token")
    echo 'new content' > "$BATS_TEST_TMPDIR/new.txt"
    echo 'old content' > "$SERVER_DATA/protected/victim"
    hold_file "/protected/hold$BATS_TEST_NUMBER"
    run xrdcp -f "$BATS_TEST_TMPDIR/new.txt" "$SERVER_URL//protected/victim?authz=$WRITE"
    release_file
    run -0 cat "$SERVER_DATA/protected/victim"
    assert_output 'old content'
}
