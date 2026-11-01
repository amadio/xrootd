#!/usr/bin/env bash

# The jwt security protocol over roots:// and HTTPS, with tokens of a local
# issuer. The issuer is a second server, which publishes the key of the shared
# credentials. It starts first, since the plugin fetches the keys when the
# data server starts. Authorization is acc.authdb on the subject of the token
# (jwt.authdb), thus the test does not check the scope of a token. The tests
# share the issuer and one data server.

load ../setup/server.bash

export X509_CERT_FILE="$CREDENTIALS/ca.pem"
export XrdSecPROTOCOL=jwt

# libcurl in the plugin does not always read SSL_CERT_FILE by itself, thus
# the plugin passes it as CURLOPT_CAINFO.
export SSL_CERT_FILE="$CREDENTIALS/ca.pem"

AUDIENCE=https://xrootd.test

setup_file() {
    require_plugin libXrdSecjwt
    require_commands curl xrdjwt-create-token

    start_server issuer http.cfg tls.cfg

    mkdir -p "$ISSUER_DATA/.well-known"
    xrdjwt-create-token jwks "$CREDENTIALS/issuer.pub" "$ISSUER_DATA/issuer.jwks" test
    printf '{"issuer":"%s","jwks_uri":"%s/issuer.jwks"}\n' "$ISSUER_HTTPS" "$ISSUER_HTTPS" \
        > "$ISSUER_DATA/.well-known/openid-configuration"

    cat > "$BATS_FILE_TMPDIR/jwt.conf" <<CONF
[Global]
audience = $AUDIENCE

[Issuer test]
issuer = $ISSUER_HTTPS
base_path = /protected
map_subject = true
CONF

    start_server http.cfg tls.cfg jwt.cfg

    mkdir -p "$SERVER_DATA/protected/subdir"
    echo 'Hello, World' > "$SERVER_DATA/protected/hello_world.txt"

    token alice "$AUDIENCE" 3600 > "$BATS_FILE_TMPDIR/alice.token"
    token bob "$AUDIENCE" 3600 > "$BATS_FILE_TMPDIR/bob.token"
    token alice https://wrong.example 3600 > "$BATS_FILE_TMPDIR/badaud.token"
    token alice "$AUDIENCE" -180 > "$BATS_FILE_TMPDIR/expired.token"
    chmod 0600 "$BATS_FILE_TMPDIR"/*.token
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    N=$BATS_TEST_NUMBER
    ROOTS="roots://localhost:$SERVER_PORT"
    echo 'jwt test' > local.txt
}

# Print a token of the issuer for the subject $1, the audience $2 and the
# lifetime $3 in seconds
token() {
    xrdjwt-create-token token "$CREDENTIALS/issuer.key" test "$ISSUER_HTTPS" "$2" "$1" "$3"
}

# Print the HTTP status of a request with the token file $1 (or none, for "")
# and the curl arguments after it
status() {
    local token=$1
    shift
    curl --silent --cacert "$CREDENTIALS/ca.pem" -o "$BATS_TEST_TMPDIR/body" -w '%{http_code}' \
        ${token:+-H "Authorization: Bearer $(<"$BATS_FILE_TMPDIR/$token.token")"} "$@"
}

#
# roots://
#

@test "a roots upload without a token fails the authentication" {
    run ! xrdcp local.txt "$ROOTS//protected/root$N.txt"
    assert_output --partial 'Auth failed'
}

@test "a roots upload with a token of alice succeeds" {
    BEARER_TOKEN_FILE="$BATS_FILE_TMPDIR/alice.token" run -0 xrdcp local.txt "$ROOTS//protected/root$N.txt"
}

@test "a roots download with a token of alice returns the file" {
    BEARER_TOKEN_FILE="$BATS_FILE_TMPDIR/alice.token" xrdcp "$ROOTS//protected/hello_world.txt" copy.txt
    run -0 cmp "$SERVER_DATA/protected/hello_world.txt" copy.txt
}

@test "a roots download with a token of bob is denied" {
    BEARER_TOKEN_FILE="$BATS_FILE_TMPDIR/bob.token" run ! xrdcp "$ROOTS//protected/hello_world.txt" copy.txt
    assert_output --partial '[3010]'
}

@test "a roots download with a token for another audience fails the authentication" {
    BEARER_TOKEN_FILE="$BATS_FILE_TMPDIR/badaud.token" run ! xrdcp "$ROOTS//protected/hello_world.txt" copy.txt
    assert_output --partial 'Auth failed'
}

@test "a roots download with an expired token fails the authentication" {
    BEARER_TOKEN_FILE="$BATS_FILE_TMPDIR/expired.token" run ! xrdcp "$ROOTS//protected/hello_world.txt" copy.txt
    assert_output --partial 'Auth failed'
}

#
# HTTPS
#

@test "a read without a token is forbidden" {
    run -0 status '' "$SERVER_HTTPS/protected/hello_world.txt"
    assert_output 403
}

@test "a read with a token of alice returns the file" {
    status alice "$SERVER_HTTPS/protected/hello_world.txt"
    assert_equal "$(<body)" 'Hello, World'
}

@test "a read with a token of bob is forbidden" {
    run -0 status bob "$SERVER_HTTPS/protected/hello_world.txt"
    assert_output 403
}

@test "a read with a token for another audience is unauthorized" {
    run -0 status badaud "$SERVER_HTTPS/protected/hello_world.txt"
    assert_output 401
}

@test "a read with an expired token is unauthorized" {
    run -0 status expired "$SERVER_HTTPS/protected/hello_world.txt"
    assert_output 401
}

@test "an upload with a token of alice creates the file" {
    run -0 status alice -X PUT -d uploaded "$SERVER_HTTPS/protected/subdir/t$N.txt"
    assert_output 201
}

@test "a read with a token of alice returns the uploaded file" {
    status alice -X PUT -d uploaded "$SERVER_HTTPS/protected/subdir/t$N.txt"
    status alice "$SERVER_HTTPS/protected/subdir/t$N.txt"
    assert_equal "$(<body)" uploaded
}
