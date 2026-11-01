#!/usr/bin/env bash

# Helpers of the tests of the fio engine (src/XrdClFio). CMake gives the fio
# binary in FIO and the engine in FIO_ENGINE: the engine works only with the
# fio of the version of its build. A test file loads this file in place of
# server.bash, or after cluster.bash or cache.bash.

if ! declare -F start_server > /dev/null; then
    load "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../setup/server.bash"
fi

# BATS stops a test and its processes after this time, thus a job which
# hangs fails its test
# shellcheck disable=SC2034 # BATS reads it
BATS_TEST_TIMEOUT=120

# fio sends many requests, and a log at the Debug level for each of them
# slows the jobs down. A test which counts requests in the client log sets
# XRD_LOGLEVEL=Debug for its job.
export XRD_LOGLEVEL=Info

# Skip the test (or all tests of the file, in setup_file) without the fio
# binary or the engine, or without jq, which reads the results of fio.
require_fio() {
    [[ -n "${FIO:-}" && -x "$FIO" ]] || skip "no fio binary for the engine"
    [[ -n "${FIO_ENGINE:-}" && -f "$FIO_ENGINE" ]] || skip "no fio engine"
    require_commands jq
}

# fio_url <url> <path>
#
# Print the URL of the file for the option filename=. fio splits filename= on
# ':', thus each colon of the URL has a backslash.
fio_url() {
    local url="$1//$2"
    echo "${url//:/\\:}"
}

# xfio [option...]
#
# Run fio with the engine and the options. The results are in the normal
# format of fio, on the standard output.
xfio() {
    "$FIO" --ioengine="external:$FIO_ENGINE" "$@"
}

# fio_json [option...]
#
# Run fio with the engine and the options, and write the results in JSON to
# fio.json in the directory of the test. The errors are on the standard
# error.
fio_json() {
    xfio --output-format=json --output="$BATS_TEST_TMPDIR/fio.json" "$@"
}

# fio_result <filter>
#
# Print the value of the jq filter for the results of the last fio_json, for
# example '.jobs[0].read.io_bytes'. fio writes its messages to the same file,
# before the JSON object.
fio_result() {
    sed -n '/^{/,$p' "$BATS_TEST_TMPDIR/fio.json" | jq -r "$1"
}

# requests <name>
#
# Print the number of requests of the type, for example kXR_sync, which the
# client log of the test has. The client logs a request at the Debug level.
requests() {
    grep -c "MsgHandler created: .*message: $1 " "$XRD_LOGFILE" || true
}

# file_size <file>: print the size of a local file in bytes
file_size() {
    wc -c < "$1" | tr -d ' '
}

# short_read_log <url>
#
# Print an iolog of fio with one read of 64 KiB at offset 0 of the file at
# the URL. For a file of less than 64 KiB, the server returns less data than
# the read asks for. In an iolog, the colons of the URL have no backslash.
short_read_log() {
    local url=${1//\\:/:}
    printf '%s\n' 'fio version 2 iolog' "$url add" "$url open" \
        "$url read 0 65536" "$url close"
}

# end_read_log <url>
#
# Print an iolog of fio with two reads of the file at the URL: 512 bytes at
# offset 0, and 64 KiB at offset 512, which goes past the end of a file of
# less than 64 KiB.
end_read_log() {
    local url=${1//\\:/:}
    printf '%s\n' 'fio version 2 iolog' "$url add" "$url open" \
        "$url read 0 512" "$url read 512 65536" "$url close"
}
