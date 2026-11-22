#!/usr/bin/env bash

# The settings of XrdClS3 in a configuration file of the client, for example
# /etc/xrootd/client.conf, with the name of the setting, as XrdCl has for its
# own settings. The variable of the shell takes the place of the setting of
# the file. The test changes the name of the sentinel of mkdir, which is the
# name of a file in the directory of the origin. Each test has its own proxy.

load xrdcls3.bash

setup_file() {
    start_s3_origin origin
}

teardown_file() {
    stop_servers
}

setup() {
    cd "$BATS_TEST_TMPDIR"
    DIR="/$BUCKET/t${BATS_TEST_NUMBER}"
    printf 'XrdClS3MkdirSentinel = .file-sentinel\n' > client.conf
}

@test "XrdClS3 reads its settings from the configuration file of the client" {
    XRD_CLCONFFILE="$PWD/client.conf" start_s3_server proxy1 proxy
    run -0 xrdfs "$PROXY1_URL" mkdir "$DIR"
    assert [ -f "$ORIGIN_DATA$DIR/.file-sentinel" ]
}

@test "a variable of the shell takes the place of the configuration file" {
    XRD_CLCONFFILE="$PWD/client.conf" XRDCLS3_MKDIRSENTINEL=.shell-sentinel \
        start_s3_server proxy2 proxy
    run -0 xrdfs "$PROXY2_URL" mkdir "$DIR"
    assert [ -f "$ORIGIN_DATA$DIR/.shell-sentinel" ]
    assert [ ! -e "$ORIGIN_DATA$DIR/.file-sentinel" ]
}
