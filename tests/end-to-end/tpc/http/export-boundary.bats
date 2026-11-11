#!/usr/bin/env bash

# The HTTP third-party-copy handler must apply the native path checks before it
# opens a local file. A native file request runs through
# XrdXrootdProtocol::rpCheck, which refuses a path with a "..", and through
# XrdXrootdProtocol::Squash, which removes the "." components before the
# authorization. The COPY handler opens the local file directly, so it must do
# the same itself.
#
# The tests cover the two ways to limit the paths of a server:
#
# - The servers "src" and "dst" export "/" below an oss.localroot. A path with
#   a ".." must not leave the local root.
# - The server "narrow" has no oss.localroot and exports only its data
#   directory, by its absolute path. A path outside the export must not reach
#   another file of the host, with or without a "..".
#
# The paths outside the exports reach files outside the data tree of a server,
# but inside its run directory, so the tests are harmless. The server "guard"
# has an authorization database which denies reads below /private.

load ../../setup/server.bash

setup_file() {
    require_plugin libXrdHttpTPC
    require_commands curl
    start_server src http.cfg http-tpc.cfg
    start_server dst http.cfg http-tpc.cfg
    start_server guard http.cfg http-tpc.cfg export-boundary-authz.cfg
    start_server narrow server-export http.cfg http-tpc.cfg

    mkdir -p "$GUARD_DATA/private"
    echo 'private content' > "$GUARD_DATA/private/secret.dat"
    echo 'public content' > "$GUARD_DATA/public.dat"
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    N=$BATS_TEST_NUMBER
    F="/t$N.dat"
    echo 'exported content' > "$SRC_DATA$F"
    # a file of the host next to the export of "narrow", and its path
    OUTSIDE="${NARROW_DATA%/data}/outside$N.dat"
}

# push <source URL> <destination URL>: request a push copy from the server of
# the source URL, with the HTTP status on standard output
push() {
    curl --silent --path-as-is -X COPY -o body.txt -w '%{http_code}' \
        -H "Destination: $2" "$1"
}

# pull <source URL> <destination URL>: request a pull copy from the server of
# the destination URL, with the HTTP status on standard output
pull() {
    curl --silent --path-as-is -X COPY -o body.txt -w '%{http_code}' \
        -H "Source: $1" "$2"
}

@test "an HTTP push copy inside the export writes the file to the destination" {
    push "$SRC_HTTP$F" "$DST_HTTP$F"
    run -0 cmp "$SRC_DATA$F" "$DST_DATA$F"
}

@test "an HTTP push copy of a path with .. fails with 403" {
    echo 'outside the export' > "$SRC_DATA/../outside$N.dat"
    run -0 push "$SRC_HTTP/../outside$N.dat" "$DST_HTTP$F"
    assert_output 403
}

@test "an HTTP push copy of a path with .. sends no content" {
    echo 'outside the export' > "$SRC_DATA/../outside$N.dat"
    push "$SRC_HTTP/../outside$N.dat" "$DST_HTTP$F"
    run ! test -e "$DST_DATA$F"
}

@test "the export check of the handler refuses an HTTP push copy of a path with .." {
    echo 'outside the export' > "$SRC_DATA/../outside$N.dat"
    push "$SRC_HTTP/../outside$N.dat" "$DST_HTTP$F"
    # match the path of this request, not the message alone, which an earlier
    # test of the shared log also wrote
    run -0 grep -E "COPY rejected: the local path is outside the exported paths.*/\.\./outside$N\.dat" <(server_log src)
}

@test "an HTTP pull copy to a path with .. fails with 403" {
    run -0 pull "$SRC_HTTP$F" "$DST_HTTP/../outside$N.dat"
    assert_output 403
}

@test "an HTTP pull copy to a path with .. creates no file outside the data tree" {
    pull "$SRC_HTTP$F" "$DST_HTTP/../outside$N.dat"
    run ! test -e "$DST_DATA/../outside$N.dat"
}

@test "an HTTP pull copy to a path with a . component writes the file without it" {
    pull "$SRC_HTTP$F" "$DST_HTTP/.$F"
    run -0 cmp "$SRC_DATA$F" "$DST_DATA$F"
}

@test "an HTTP push copy of a public file passes the authorization" {
    push "$GUARD_HTTP/public.dat" "$DST_HTTP$F"
    run -0 cmp "$GUARD_DATA/public.dat" "$DST_DATA$F"
}

@test "an HTTP push copy of a private file fails with 403" {
    run -0 push "$GUARD_HTTP/private/secret.dat" "$DST_HTTP$F"
    assert_output 403
}

@test "an HTTP push copy of a private file through a . component fails with 403" {
    run -0 push "$GUARD_HTTP/./private/secret.dat" "$DST_HTTP$F"
    assert_output 403
}

@test "an HTTP push copy of a private file through a . component sends no content" {
    push "$GUARD_HTTP/./private/secret.dat" "$DST_HTTP$F"
    run ! test -e "$DST_DATA$F"
}

#
# without oss.localroot: the server "narrow" exports only its data directory
#

@test "an HTTP push copy inside a narrow export writes the file to the destination" {
    echo 'exported content' > "$NARROW_DATA$F"
    push "$NARROW_HTTP$NARROW_DATA$F" "$DST_HTTP$F"
    run -0 cmp "$NARROW_DATA$F" "$DST_DATA$F"
}

@test "an HTTP push copy of a path outside a narrow export fails with 403" {
    echo 'outside the export' > "$OUTSIDE"
    run -0 push "$NARROW_HTTP$OUTSIDE" "$DST_HTTP$F"
    assert_output 403
}

@test "an HTTP push copy of a path outside a narrow export sends no content" {
    echo 'outside the export' > "$OUTSIDE"
    push "$NARROW_HTTP$OUTSIDE" "$DST_HTTP$F"
    run ! test -e "$DST_DATA$F"
}

@test "the export check of the handler refuses an HTTP push copy of a path outside a narrow export" {
    echo 'outside the export' > "$OUTSIDE"
    push "$NARROW_HTTP$OUTSIDE" "$DST_HTTP$F"
    run -0 grep -F "COPY rejected: the local path is outside the exported paths $OUTSIDE" <(server_log narrow)
}

@test "an HTTP push copy of a path with .. out of a narrow export fails with 403" {
    echo 'outside the export' > "$OUTSIDE"
    run -0 push "$NARROW_HTTP$NARROW_DATA/../outside$N.dat" "$DST_HTTP$F"
    assert_output 403
}

@test "an HTTP push copy of a path with .. out of a narrow export sends no content" {
    echo 'outside the export' > "$OUTSIDE"
    push "$NARROW_HTTP$NARROW_DATA/../outside$N.dat" "$DST_HTTP$F"
    run ! test -e "$DST_DATA$F"
}

@test "an HTTP pull copy to a path outside a narrow export fails with 403" {
    run -0 pull "$SRC_HTTP$F" "$NARROW_HTTP$OUTSIDE"
    assert_output 403
}

@test "an HTTP pull copy to a path outside a narrow export creates no file" {
    pull "$SRC_HTTP$F" "$NARROW_HTTP$OUTSIDE"
    run ! test -e "$OUTSIDE"
}

@test "an HTTP pull copy to a path with .. out of a narrow export creates no file" {
    pull "$SRC_HTTP$F" "$NARROW_HTTP$NARROW_DATA/../outside$N.dat"
    run ! test -e "$OUTSIDE"
}
