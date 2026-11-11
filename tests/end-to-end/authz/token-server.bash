#!/usr/bin/env bash

# Helper for the tests of the identity of a token. The server has the SciTokens
# module in pass-through mode (see token-server.cfg).

load ../setup/server.bash

# start_token_server [snippet.cfg...]
#
# Start the token issuer and the server "server" with the snippet
# token-server.cfg and the snippets of the test. Make the file
# /protected/hello, and the token read.token, which may read /protected, in
# $BATS_FILE_TMPDIR.
start_token_server() {
    require_plugin libXrdAccSciTokens

    start_issuer

    # The token maps to the default user "nobody"; only "nobody" may read.
    printf 'u nobody /protected lr\n' > "$(_setup_tmpdir)/token-server.authdb"

    start_server http.cfg tls.cfg token-server.cfg "$@"

    mkdir -p "$SERVER_DATA/protected"
    echo 'protected content' > "$SERVER_DATA/protected/hello"

    make_token storage.read:/protected > "$BATS_FILE_TMPDIR/read.token"
    chmod 0600 "$BATS_FILE_TMPDIR/read.token"
}
