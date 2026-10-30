#!/usr/bin/env bash

# Batch copying and JSON manifest support in xrdcp.

load ../setup/server.bash

setup() {
    cd "$BATS_TEST_TMPDIR"
}

@test "xrdcp --jobs-file requires an existing manifest" {
    run ! xrdcp --jobs-file nonexistent.jsonl
    assert_output --partial "Unable to open the jobs manifest."
}

@test "xrdcp --jobs-file rejects combination with operands" {
    run ! xrdcp --jobs-file /dev/null src dst
    assert_output --partial "'--jobs-file' cannot be combined with operands or '--infiles'."
}

@test "xrdcp --checksum-mode requires a verifying --cksum argument" {
    run ! xrdcp --checksum-mode source src dst
    assert_output --partial "'--checksum-mode' requires a verifying '--cksum' argument."
}

@test "xrdcp batch test suite passes" {
    local http_plugin=""
    if [[ -f "$BINARY_DIR/src/XrdClHttp/libXrdClHttp-6.so" ]]; then
        http_plugin="$BINARY_DIR/src/XrdClHttp/libXrdClHttp-6.so"
    elif [[ -f "$BINARY_DIR/src/XrdClHttp/libXrdClHttp-6.dylib" ]]; then
        http_plugin="$BINARY_DIR/src/XrdClHttp/libXrdClHttp-6.dylib"
    fi

    run -0 python3 "$BATS_TEST_DIRNAME/copy_batch.py" "$(command -v xrdcp)" "$http_plugin"
}
