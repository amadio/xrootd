#!/usr/bin/env bash

# The server gives the name of the user to an external checksum program. With
# a token, this is the name that the token module mapped. The mapped name
# belongs to one request, thus the checksum query must authorize itself to
# get it, and a query without a token must fail.
#
# The checksum program of the test records the name that it gets.

load token-server.bash

setup_file() {
    cat > "$BATS_FILE_TMPDIR/checksum.sh" <<SCRIPT
#!/bin/sh
# arguments: <algorithm> <path> <tident> <name>
echo "\$4" > "$BATS_FILE_TMPDIR/checksum.user"
echo 0badc0de
SCRIPT
    chmod +x "$BATS_FILE_TMPDIR/checksum.sh"

    start_token_server token-checksum.cfg
}

teardown_file() {
    stop_servers
}

@test "the checksum program gets the mapped name of the token" {
    TOKEN=$(<"$BATS_FILE_TMPDIR/read.token")
    run -0 xrdfs "$SERVER_URL" query checksum "/protected/hello?authz=$TOKEN"
    assert_equal "$(<"$BATS_FILE_TMPDIR/checksum.user")" nobody
}

@test "a checksum query without a token is not authorized" {
    run ! xrdfs "$SERVER_URL" query checksum /protected/hello
    assert_output --partial '[3010]'
}
