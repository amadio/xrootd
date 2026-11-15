#!/usr/bin/env bash

# A redirect of a request over HTTPS to HTTP on the same server. The response
# must close the TLS connection, also for a keep-alive request. Each test has
# its own server.

load ../setup/server.bash

setup_file() {
    require_plugin libXrdHttp
    require_commands curl python3
}

setup() {
    start_server http.cfg tls.cfg redirect.cfg
}

teardown() {
    stop_servers
}

# The response headers of a keep-alive GET over TLS, read until the server
# closes the connection
redirect() {
    python3 "$BATS_TEST_DIRNAME/https-redirect.py" "$SERVER_PORT" "$CREDENTIALS/ca.pem" | tr -d '\r'
}

@test "a GET over HTTPS gets a redirect to HTTP" {
    run -0 redirect
    assert_line 'HTTP/1.1 302 Found'
}

@test "the redirect closes a keep-alive connection" {
    run -0 redirect
    assert_line --regexp '^[Cc]onnection: [Cc]lose$'
}

# The Location of the redirect, an HTTP URL with a token of the identity of
# the client
location() {
    redirect | sed -n 's/^[Ll]ocation: //p'
}

# The HTTP status of a GET of the URL
status() {
    curl --silent -o /dev/null --write-out '%{http_code}' "$1"
}

@test "the token of the redirect gives access over HTTP" {
    echo 'content' > "$SERVER_DATA/test"
    run -0 status "$(location)"
    assert_output 200
}

# The server applied a role and groups of the URL to the identity of the
# client, but the token did not sign them
@test "a role added to the token of the redirect is rejected" {
    echo 'content' > "$SERVER_DATA/test"
    run -0 status "$(location)&xrdhttprole=admin"
    assert_output 400
}

@test "groups added to the token of the redirect are rejected" {
    echo 'content' > "$SERVER_DATA/test"
    run -0 status "$(location)&xrdhttpgrps=/admin"
    assert_output 400
}
