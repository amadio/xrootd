#!/usr/bin/env bash

# Prefer command-line tools from the active CMake build when CTest provides it.
if [[ -n "${BINARY_DIR:-}" ]]; then
    export PATH="${BINARY_DIR}/bin:${PATH}"
fi

# Start xrootd in the background and wait until it has completed its
# initialization, so that a test never races with the server startup.
launch_xrootd() {
    local config=$1
    local name=$2
    local log="$BATS_TEST_TMPDIR/$name.log"
    local pidfile="$BATS_TEST_TMPDIR/$name.pid"

    pushd "$(pwd)" 1>/dev/null
    cd "$BATS_TEST_TMPDIR"
    BATS_TEST_DIRNAME=${BATS_TEST_DIRNAME} BATS_SUITE_TMPDIR=${BATS_SUITE_TMPDIR} NAME=$name \
        xrootd -b -c "${BATS_TEST_DIRNAME}/$config" -l "$name.log" -s "$name.pid"
    popd 1>/dev/null

    # xrootd logs one of these lines at the end of its configuration
    for _ in {1..200}; do
        if grep -qs 'initialization completed' "$log"; then
            return 0
        fi

        if grep -qs 'initialization failed' "$log"; then
            break
        fi

        # the pid file exists but the daemon is gone
        if [[ -s "$pidfile" ]] && ! kill -0 "$(<"$pidfile")" 2>/dev/null; then
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

# Stop every daemon started by the test and wait until it exits, so that the
# next test can bind the same ports.
kill_pid_files() {
    local pidfile pid

    while IFS= read -r pidfile; do
        pid=$(<"$pidfile")

        if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null

            for _ in {1..100}; do
                kill -0 "$pid" 2>/dev/null || break
                sleep 0.05
            done

            # the graceful shutdown timed out
            kill -9 "$pid" 2>/dev/null
        fi

        rm -f "$pidfile"
    done < <(find "$BATS_TEST_TMPDIR" -name '*.pid' -type f)

    return 0
}

bats::on_failure() {
    print_log_files
}
