#!/usr/bin/env bash

# Helpers of the benchmarks. A benchmark runs a fio job with the engine of
# tests/end-to-end/fio against the servers of the setups of the end-to-end
# tests, and prints its results as measurements of CTest. CDash keeps the
# measurements of each submission, thus a series of submissions shows the
# performance of the client and the server over time.
#
# A benchmark runs alone (RUN_SERIAL), thus the CPU time of the processes is
# the cost of the job, and not of other tests.

load ../end-to-end/fio/fio.bash

# The duration of a job with a time limit, in seconds
: "${BENCHMARK_RUNTIME:=5}"

# The options of fio for a job with a time limit
# shellcheck disable=SC2034 # the benchmark files use it
TIMED=(--time_based "--runtime=$BENCHMARK_RUNTIME")

# measure <name> <value>
#
# Print a measurement of CTest on the output of BATS. CTest removes it from
# the output of the test and sends it to CDash.
measure() {
    printf '<DartMeasurement type="numeric/double" name="%s">%s</DartMeasurement>\n' "$1" "$2" >&3
}

# cpu_ticks <name>
#
# Print the CPU time (user and system) of the server in clock ticks, from
# /proc. The fio engine is for Linux only, thus /proc exists.
cpu_ticks() {
    local stat
    stat=$(<"/proc/$(<"$(_setup_rundir "$1")/xrootd.pid")/stat")
    # the fields after the command name, which is in parentheses, start with
    # the state, field 3; utime and stime are the fields 14 and 15
    stat=${stat##*) }
    # shellcheck disable=SC2086 # split the fields
    set -- $stat
    echo $(( ${12} + ${13} ))
}

# origin_bytes <records> <lfn>
#
# Print the bytes which a cache got from its origin for the file: the sum of
# b_todisk (the bytes of the blocks which it wrote to disk, prefetch included)
# and b_bypass (the bytes which it passed on without a block) of the
# file_close records of the file in the g-stream. The cache sends a record
# when it closes the file, after the client, and the g-stream flushes each
# second, thus this waits up to 20 s for the first record.
origin_bytes() {
    local records=$1 lfn=$2

    wait_for 20 grep -qF "\"lfn\":\"$lfn\"" "$records" || {
        echo "no file_close record of $lfn" >&2
        return 1
    }
    grep -o '{"event":"file_close"[^}]*}' "$records" | grep -F "\"lfn\":\"$lfn\"" |
        sed -n 's/.*"b_bypass":\([0-9]*\),"b_todisk":\([0-9]*\),.*/\1 \2/p' |
        awk '{ s += $1 + $2 } END { print s + 0 }'
}

# _ratio <a> <b> [scale]: print a * scale / b with 3 decimals, 0 for b = 0
_ratio() {
    awk -v a="$1" -v b="$2" -v s="${3:-1}" 'BEGIN { printf "%.3f\n", b ? a * s / b : 0 }'
}

# bench [option...]
#
# Run a fio job with the options, and print its results as measurements:
# for the reads and the writes, the bandwidth, the IOPS and the latency;
# the CPU time of the client (the whole fio process, with the threads of
# XrdCl) and of each server of BENCHMARK_SERVERS (default: server), in
# percent of the time of the job and in seconds per GiB of data. With
# BENCHMARK_GSTREAM, the records of the g-stream of a cache, and
# BENCHMARK_LFN, the file of the job, also the read amplification: the bytes
# of the origin for each byte which fio read. Then the cache must start the
# benchmark without the file.
bench() {
    local servers=${BENCHMARK_SERVERS:-server} s i ticks=() origin
    local tck user sys real cpu bytes rbytes

    tck=$(getconf CLK_TCK)

    for s in $servers; do
        ticks+=("$(cpu_ticks "$s")")
    done

    # bash measures the CPU time of fio and of all its threads
    local TIMEFORMAT='%3U %3S %3R'
    { time fio_json --group_reporting --lat_percentiles=1 "$@" \
        2> "$BATS_TEST_TMPDIR/fio.err"; } 2> "$BATS_TEST_TMPDIR/time.txt" || {
        cat "$BATS_TEST_TMPDIR/fio.err" >&2
        return 1
    }
    read -r user sys real < "$BATS_TEST_TMPDIR/time.txt"

    rbytes=$(fio_result '.jobs[0].read.io_bytes')
    bytes=$(( rbytes + $(fio_result '.jobs[0].write.io_bytes') ))

    _measure_ddir read Read
    _measure_ddir write Write

    cpu=$(awk -v u="$user" -v s="$sys" 'BEGIN { print u + s }')
    measure "Client CPU (%)" "$(_ratio "$cpu" "$real" 100)"
    measure "Client CPU per GiB (s)" "$(_ratio "$cpu" "$bytes" 1073741824)"

    i=0
    for s in $servers; do
        cpu=$(_ratio $(( $(cpu_ticks "$s") - ticks[i] )) "$tck")
        # the name with a capital letter, for example "Cache"
        s="$(printf '%s' "${s:0:1}" | tr '[:lower:]' '[:upper:]')${s:1}"
        measure "$s CPU (%)" "$(_ratio "$cpu" "$real" 100)"
        measure "$s CPU per GiB (s)" "$(_ratio "$cpu" "$bytes" 1073741824)"
        i=$(( i + 1 ))
    done

    if [[ -n "${BENCHMARK_GSTREAM:-}" ]]; then
        origin=$(origin_bytes "$BENCHMARK_GSTREAM" "$BENCHMARK_LFN") || return 1
        measure "Origin data (MiB)" "$(_ratio "$origin" 1048576)"
        measure "Read amplification" "$(_ratio "$origin" "$rbytes")"
    fi
}

# _measure_ddir <read|write> <Label>: the measurements of one direction
_measure_ddir() {
    local d=$1 label=$2

    (( $(fio_result ".jobs[0].$d.io_bytes") > 0 )) || return 0

    measure "$label bandwidth (MiB/s)" "$(_ratio "$(fio_result ".jobs[0].$d.bw_bytes")" 1048576)"
    measure "$label IOPS" "$(fio_result ".jobs[0].$d.iops | . * 1000 | round / 1000")"
    measure "$label latency mean (us)" "$(_ratio "$(fio_result ".jobs[0].$d.lat_ns.mean")" 1000)"
    measure "$label latency p50 (us)" "$(_ratio "$(fio_result ".jobs[0].$d.lat_ns.percentile.\"50.000000\"")" 1000)"
    measure "$label latency p99 (us)" "$(_ratio "$(fio_result ".jobs[0].$d.lat_ns.percentile.\"99.000000\"")" 1000)"
}

# root_iolog <url> <clusters> [passes]
#
# Print an iolog of fio which reads a file like an analysis with ROOT reads
# a TTree: the file has clusters of entries, and each cluster has one basket
# of each of 40 branches, of 64 KiB to 448 KiB, thus 9.8 MiB. The analysis
# reads 10 of the branches, 24.5% of the data, thus 10 baskets of each
# cluster, in the order of the file. The baskets do not start on a boundary
# of the blocks of a cache. The analysis reads the file the number of passes
# (default 1); fio repeats no replay of an iolog for loops=. The URL has no
# backslashes.
root_iolog() {
    local url=$1 clusters=$2 passes=${3:-1}

    echo 'fio version 2 iolog'
    echo "$url add"
    echo "$url open"
    awk -v url="$url" -v clusters="$clusters" -v passes="$passes" 'BEGIN {
        for (p = 0; p < passes; p++) {
            off = 0
            for (c = 0; c < clusters; c++) {
                for (b = 0; b < 40; b++) {
                    size = 65536 + ((b * 7919) % 17) * 24576
                    if (b % 4 == 1)
                        printf "%s read %d %d\n", url, off, size
                    off += size
                }
            }
        }
    }'
    echo "$url close"
}
