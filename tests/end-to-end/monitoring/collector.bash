#!/usr/bin/env bash

# Helpers of the tests of xrdmoncollect. A test file loads this file after
# setup/server.bash. Each daemon runs in the directory <name> of the current
# context, with its output in daemon.log and its pid in daemon.pid there, thus
# stop_servers stops it.

# port_in_use <udp|tcp> <port>
#
# Succeed if a process has bound the port, on any address of either IP stack.
port_in_use() {
    python3 -c '
import socket, sys
kind = socket.SOCK_DGRAM if sys.argv[1] == "udp" else socket.SOCK_STREAM
s = socket.socket(socket.AF_INET6, kind)
s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
try:
    s.bind(("::", int(sys.argv[2])))
except OSError:
    sys.exit(0)
sys.exit(1)' "$1" "$2"
}

# _daemon_started <pid> <udp|tcp> <port>: the daemon exited, or bound the port
_daemon_started() {
    ! is_running "$1" || port_in_use "$2" "$3"
}

# run_daemon <name> <udp|tcp> <port> <command> [argument...]
#
# Run the command in the background, with each @PORT@ of its arguments
# replaced by the port, and wait until it binds the port. Fail if the daemon
# exits first. The output of each run goes to the end of daemon.log. To
# restart a stopped daemon, run it again with its name and port.
run_daemon() {
    local name=$1 proto=$2 port=$3 dir arg pid
    shift 3
    # a daemon that runs again keeps its directory, also when the file
    # started it and a test restarts it
    dir=$(_setup_rundir "$name" 2>/dev/null) || dir="$(_setup_tmpdir)/$name"
    mkdir -p "$dir"

    local args=()
    for arg in "$@"; do
        args+=("${arg//@PORT@/$port}")
    done

    # the daemon must not keep the descriptors 3 and 4, on which BATS reads
    # the output of the test until all holders close them
    ( "${args[@]}" < /dev/null >> "$dir/daemon.log" 2>&1 3>&- 4>&- &
      echo $! > "$dir/daemon.pid" )
    pid=$(<"$dir/daemon.pid")

    wait_for 10 _daemon_started "$pid" "$proto" "$port" || true
    if is_running "$pid" && port_in_use "$proto" "$port"; then
        return 0
    fi

    stop_pid "$pid"
    rm -f "$dir/daemon.pid"
    return 1
}

# start_daemon <name> <udp|tcp> <command> [argument...]
#
# Run the daemon (see run_daemon) on a random port from 21000 to 31999, and
# try another port up to 5 times. Export, with <NAME> the name in upper case,
# <NAME>_PORT, the port, and <NAME>_LOG, the output of the daemon.
start_daemon() {
    local name=$1 proto=$2 dir var port
    shift 2
    dir="$(_setup_tmpdir)/$name"
    var=$(_setup_var "$name")

    for _ in 1 2 3 4 5; do
        port=$(( 21000 + RANDOM % 11000 ))
        port_in_use "$proto" "$port" && continue

        if run_daemon "$name" "$proto" "$port" "$@"; then
            export "${var}_PORT=$port" "${var}_LOG=$dir/daemon.log"
            return 0
        fi
    done

    echo "start_daemon: '$name' failed to start" >&2
    [[ -f "$dir/daemon.log" ]] && sed "s|^|[$name] |" "$dir/daemon.log" >&2
    return 1
}

# start_mock <name> [reject]
#
# Start the OTLP/HTTP receiver otlp_mock.py. With "reject", the mock refuses
# its next POST while the file <NAME>_REJECT exists. Export <NAME>_PORT, the
# port, and <NAME>_OUT, the file of the requests.
start_mock() {
    local name=$1 dir var
    dir="$(_setup_tmpdir)/$name"
    var=$(_setup_var "$name")
    mkdir -p "$dir"
    : > "$dir/captured"

    ( python3 "$BATS_TEST_DIRNAME/otlp_mock.py" "$dir/port" "$dir/captured" \
          ${2:+"$dir/reject-next"} < /dev/null > "$dir/daemon.log" 2>&1 3>&- 4>&- &
      echo $! > "$dir/daemon.pid" )

    if ! wait_for 10 test -s "$dir/port"; then
        echo "start_mock: '$name' failed to start" >&2
        sed "s|^|[$name] |" "$dir/daemon.log" >&2
        return 1
    fi

    export "${var}_PORT=$(<"$dir/port")" "${var}_OUT=$dir/captured" \
        "${var}_REJECT=$dir/reject-next"
}

# matching <regex>...
#
# Print the documents in the file $DOCUMENTS that match all the extended
# regular expressions, in any order of the keys. Fail if none matches.
matching() {
    local docs regex
    docs=$(<"$DOCUMENTS")

    for regex in "$@"; do
        docs=$(grep -E -- "$regex" <<< "$docs" || true)
    done

    [[ -n "$docs" ]] && echo "$docs"
}

# drive_until <regex>... -- <command> [argument...]
#
# Run the command each second until a document matches all the regular
# expressions, for at most 60 s. Monitoring is UDP, thus a single run can be
# lost.
drive_until() {
    local regexes=()

    while [[ "$1" != -- ]]; do
        regexes+=("$1")
        shift
    done
    shift

    for _ in {1..60}; do
        "$@" > /dev/null 2>&1 || true
        matching "${regexes[@]}" > /dev/null && return 0
        sleep 1
    done

    echo "no document matches: ${regexes[*]}" >&2
    return 1
}

# has_plugin <name>: succeed if the build has the plugin
has_plugin() {
    local lib

    [[ -n "${BINARY_DIR:-}" ]] || return 1

    for lib in "$BINARY_DIR"/lib*/"$1"{,-*}.{so,dylib}; do
        [[ -e "$lib" ]] && return 0
    done

    return 1
}
