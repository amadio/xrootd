#!/usr/bin/env bash

# A mapped token identity must not outlive its authorization. The token modules
# write the mapped local name as "request.name" on the connection's security
# record, and XrdAcc prefers that name over the connection identity. If nothing
# clears the name, a later authorization without a token inherits the file
# permissions of the earlier mapped name. This applies to a later request on
# the connection, and to a later path of the same kXR_statx request.
#
# The issuer maps a valid token to the user "nobody", which the authorization
# database allows to read /protected. The anonymous identity is denied. The
# test sends a token request and then a tokenless request on the same keep-alive
# connection, and checks that the tokenless request is denied.

load token-server.bash

export X509_CERT_FILE="$CREDENTIALS/ca.pem"

setup_file() {
    require_commands curl python3

    start_token_server

    # A token for one file only, for the tests of kXR_statx
    echo 'other content' > "$SERVER_DATA/protected/other"
    make_token storage.read:/protected/hello > "$BATS_FILE_TMPDIR/hello.token"
    chmod 0600 "$BATS_FILE_TMPDIR/hello.token"
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    TOKEN=$(<"$BATS_FILE_TMPDIR/read.token")
    URL="$SERVER_HTTPS/protected/hello"
}

@test "a read with a valid token is allowed" {
    run -0 curl --silent --cacert "$CREDENTIALS/ca.pem" -o /dev/null -w '%{http_code}' \
        -H "Authorization: Bearer $TOKEN" "$URL"
    assert_output 200
}

@test "a tokenless read on a fresh connection is forbidden" {
    run -0 curl --silent --cacert "$CREDENTIALS/ca.pem" -o /dev/null -w '%{http_code}' "$URL"
    assert_output 403
}

# reuse: a read with the token, then a read without a token, on one keep-alive
# connection. Print the status of each request and, for the second one, the
# number of new connections, which is 0 when curl reused the connection.
reuse() {
    curl --silent --http1.1 --cacert "$CREDENTIALS/ca.pem" \
        -o /dev/null -w 'first=%{http_code}\n' \
        -H "Authorization: Bearer $TOKEN" "$URL" \
        --next --http1.1 --cacert "$CREDENTIALS/ca.pem" \
        -o /dev/null -w 'second=%{http_code} connects=%{num_connects}\n' "$URL"
}

@test "a read with a token and a read without a token share one connection" {
    run -0 reuse
    assert_line --regexp '^second=[0-9]+ connects=0$'
}

@test "a tokenless read does not inherit the mapped name of an earlier request" {
    run -0 reuse
    assert_line 'first=200'
    assert_line 'second=403 connects=0'
}

#
# native root://, with the token in the "authz" opaque of each request
#

# Run the commands on one xrdfs connection: its interactive mode reads them
# from standard input, so they share the connection and its security record.
xrdfs_session() {
    printf '%s\n' "$@" | xrdfs "$SERVER_URL"
}

@test "a native read with a token in the opaque is allowed" {
    run -0 xrdfs_session "cat /protected/hello?authz=$TOKEN"
    assert_output --partial 'protected content'
}

@test "a native tokenless read on a fresh connection is not authorized" {
    run ! xrdfs "$SERVER_URL" cat /protected/hello
    assert_output --partial '[3010]'
}

@test "the commands of one xrdfs session share one connection" {
    run xrdfs_session "cat /protected/hello?authz=$TOKEN" "cat /protected/hello"
    run -0 grep -c 'XrootdXeq: .* login as' <(server_log server)
    assert_output 1
}

@test "a native tokenless read does not inherit the mapped name of an earlier request" {
    # a read with the token, then a read without it, on one connection; only
    # the first prints the content
    run xrdfs_session "cat /protected/hello?authz=$TOKEN" "cat /protected/hello"
    assert_equal "$(grep -c 'protected content' <<<"$output")" 1
}

@test "a native tokenless read after a read with a token is not authorized" {
    run xrdfs_session "cat /protected/hello?authz=$TOKEN" "cat /protected/hello"
    assert_output --partial '[3010]'
}

#
# kXR_statx, which authorizes each path of one request. No client of XRootD
# sends this request with a token, thus the raw client sends it.
#

statx() {
    python3 "$SETUP_DIR/raw-client.py" localhost "$SERVER_PORT" statx "$@"
}

@test "a statx of a path with its token is allowed" {
    HELLO=$(<"$BATS_FILE_TMPDIR/hello.token")
    run -0 statx "/protected/hello?authz=$HELLO"
    assert_output --regexp '^ok [0-9]+$'
}

@test "a statx of a path without a token is not authorized" {
    run -0 statx /protected/other
    assert_output --partial 'error 3010'
}

@test "a path of a statx does not inherit the mapped name of an earlier path" {
    HELLO=$(<"$BATS_FILE_TMPDIR/hello.token")
    run -0 statx "/protected/hello?authz=$HELLO" /protected/other
    assert_output --partial 'error 3010'
}
