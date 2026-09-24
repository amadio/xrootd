#!/usr/bin/env bash

# Prefer command-line tools from the active CMake build when CTest provides it.
if [[ -n "${BINARY_DIR:-}" ]]; then
    export PATH="${BINARY_DIR}/bin:${PATH}"
fi

# Start xrootd and wait until it has completed its initialization, so that a
# test never races with the server startup. xrootd runs in the foreground, as a
# child of the test shell, so that kill_pid_files can wait for its exit.
launch_xrootd() {
    local config=$1
    local name=$2
    local log="$BATS_TEST_TMPDIR/$name.log"
    local pid

    pushd "$(pwd)" 1>/dev/null
    cd "$BATS_TEST_TMPDIR"
    # bats waits for the standard streams and fd 3 to close, thus the server
    # must not keep them open
    BATS_TEST_DIRNAME=${BATS_TEST_DIRNAME} BATS_SUITE_TMPDIR=${BATS_SUITE_TMPDIR} NAME=$name \
        xrootd -c "${BATS_TEST_DIRNAME}/$config" -l "$name.log" -s "$name.pid" \
        </dev/null >"$name.stdout.log" 2>&1 3>&- &
    pid=$!
    XROOTD_PIDS+=("$pid")
    popd 1>/dev/null

    # xrootd logs one of these lines at the end of its configuration; the
    # components log similar lines before it, thus match the whole line
    for _ in {1..200}; do
        if grep -qsE '^------ xrootd .* initialization completed' "$log"; then
            return 0
        fi

        if grep -qsE '^------ xrootd .* initialization failed' "$log"; then
            break
        fi

        # the server exited before it logged the result
        if ! jobs -rp | grep -qx "$pid"; then
            break
        fi

        sleep 0.05
    done

    echo "xrootd '$name' failed to start" >&2
    [[ -f "$log" ]] && cat "$log" >&2
    return 1
}

print_log_files() {
    if [[ -z "${BATS_SKIP_SERVER_LOGS}" ]]; then
        for file in $(find $BATS_TEST_TMPDIR -name '*.log' -type f ! -empty); do
            name="${file##*/}"
            name="${name%.*}"
            printf '\n'
            sed "s|^|[$name] |" "$file"
            printf '\n'
        done
    fi
}

# Stop every server started by the test. wait returns only once the whole
# process has exited, thus closed its sockets, so the next test can bind the
# same ports at once.
#
# Teardown runs on a normal exit, on Ctrl-C, and on a CTest timeout. A SIGTERM
# or SIGKILL sent only to the top bats process leaves the servers running,
# since bats then deletes its run directory and cannot run teardown. To stop a
# run, signal its whole process group, as Ctrl-C does.
kill_pid_files() {
    local pid

    for pid in "${XROOTD_PIDS[@]}"; do
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    done
    XROOTD_PIDS=()

    find "$BATS_TEST_TMPDIR" -name '*.pid' -type f -delete

    return 0
}

bats::on_failure() {
    print_log_files
}

# Ask the server at the given URL for a macaroon with full access, over TLS
# verified against the test CA. Fails if the server returns no macaroon.
request_macaroon() {
    local url=$1

    curl --fail --silent --show-error \
        --cacert "$BATS_SUITE_TMPDIR/ca.pem" \
        --cert "$BATS_SUITE_TMPDIR/client.crt" --key "$BATS_SUITE_TMPDIR/client.key" \
        -X POST -H 'Content-Type: application/macaroon-request' \
        -d '{ "caveats": [ "activity:READ_METADATA,UPDATE_METADATA,LIST,DOWNLOAD,UPLOAD,MANAGE,DELETE" ], "validity": "PT1H" }' \
        "$url" | jq -er .macaroon
}
