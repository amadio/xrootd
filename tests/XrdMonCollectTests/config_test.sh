#!/usr/bin/env bash
# Exercises the xrdmoncollect INI configuration file: a value is read from the
# file, a command-line option overrides it, and malformed/missing files are
# reported. Only deterministic, non-binding paths are checked (an out-of-range
# port makes the validated start-up fail fast without opening a socket).

set -u

BIN=${1:?usage: config_test.sh <path-to-xrdmoncollect>}
TMP=$(mktemp -d)
trap 'rm -rf "${TMP}"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# Start the collector with the given arguments and check that it is still
# running (i.e. got past parsing/validation and into the receive loop) shortly
# afterwards, then shut it down. This deliberately avoids timeout(1)'s exit
# code: the 124-on-timeout convention is GNU-specific (busybox on Alpine
# propagates the child's status, and the collector exits 0 on SIGTERM).
starts() {
	local pid alive=0
	"${BIN}" "$@" & pid=$!
	sleep 1
	kill -0 "${pid}" 2>/dev/null && alive=1
	kill "${pid}" 2>/dev/null
	wait "${pid}" 2>/dev/null
	test "${alive}" -eq 1
}

# 1. A value from the file takes effect: an out-of-range port in the config is
#    applied and rejected by the same validation as a command-line port.
cat > "${TMP}/badport.cfg" <<EOF
[xrdmoncollect]
port = 70000
EOF
out=$("${BIN}" -c "${TMP}/badport.cfg" 2>&1)
test $? -ne 0 || fail "out-of-range config port should fail"
grep -q "valid -p" <<<"${out}" || fail "expected port validation error, got: ${out}"

# 2. A command-line option overrides the file: a valid -p replaces the bad
#    config port, so validation passes and the collector starts (it would
#    otherwise bind and run, so 'starts' stops it after probing).
starts -c "${TMP}/badport.cfg" -p 9931 \
	|| fail "CLI -p should override config and start"

# 3. A malformed config is a hard error naming the line.
printf '[xrdmoncollect\nport = 9931\n' > "${TMP}/bad.cfg"
out=$("${BIN}" -c "${TMP}/bad.cfg" 2>&1)
test $? -ne 0 || fail "malformed config should fail"
grep -qi "parse error" <<<"${out}" || fail "expected parse error, got: ${out}"

# 4. An explicitly named but unreadable config is a hard error.
out=$("${BIN}" -c "${TMP}/does-not-exist.cfg" 2>&1)
test $? -ne 0 || fail "missing -c file should fail"
grep -qi "cannot open" <<<"${out}" || fail "expected open error, got: ${out}"

# 5. A valid config is honoured: the file alone supplies the port and the
#    collector starts (binds), which we again stop quickly.
cat > "${TMP}/ok.cfg" <<EOF
[xrdmoncollect]
port = 9932
no-resolve = true
max-memory = 1G
site = EXAMPLE-SITE
EOF
starts -c "${TMP}/ok.cfg" || fail "valid config should start the collector"

# 5b. The site knob has to reach the metrics registry, and it can only do that
#     before the first subsystem is created -- setGlobalLabels() declines the
#     change afterwards. Scrape and look for the label rather than trusting that
#     the collector merely started.
if command -v curl >/dev/null 2>&1; then
	"${BIN}" -c "${TMP}/ok.cfg" -p 9934 --metrics-port 9935 \
	         >"${TMP}/site.log" 2>&1 </dev/null &
	pid=$!
	sleep 1
	got=$(curl -sf http://localhost:9935/metrics || true)
	kill "${pid}" 2>/dev/null || true
	wait "${pid}" 2>/dev/null || true
	case "${got}" in
	  *'site="EXAMPLE-SITE"'*) ;;
	  *) fail "site from the config file did not reach the metrics" ;;
	esac
	# The "process" subsystem is registered from a separate call placed after
	# every other one (see registerProcessMetrics), so it is the piece most
	# easily lost to a refactor. Its absence is silent otherwise.
	case "${got}" in
	  *'xrootd_process_resident_memory_bytes{site="EXAMPLE-SITE"}'*) ;;
	  *) fail "process RSS metric missing from the exposition" ;;
	esac
fi

# 6. A filter rule keyed on something that is not a known field is a hard error
#    naming the key: a rule that silently matches nothing is indistinguishable
#    from one that works, until documents go missing.
printf '[xrdmoncollect]\nport = 9933\n[filter "x"]\nusr = alice\n' > "${TMP}/badkey.cfg"
out=$("${BIN}" -c "${TMP}/badkey.cfg" 2>&1)
test $? -ne 0 || fail "unknown filter key should fail"
grep -q "usr" <<<"${out}" || fail "expected the bad key to be named, got: ${out}"

# 7. A ~pattern that does not compile is rejected at start-up, not per document.
printf '[xrdmoncollect]\nport = 9933\n[filter "x"]\nuser = ~([\n' > "${TMP}/badre.cfg"
"${BIN}" -c "${TMP}/badre.cfg" >/dev/null 2>&1 && fail "bad regex should fail"

# 8. An unnamed section, an unknown action, and a rule with no conditions (which
#    would match every document) are all rejected.
printf '[xrdmoncollect]\nport = 9933\n[filter]\nuser = alice\n' > "${TMP}/noname.cfg"
"${BIN}" -c "${TMP}/noname.cfg" >/dev/null 2>&1 && fail "[filter] needs a name"
printf '[xrdmoncollect]\nport = 9933\n[filter "x"]\nuser = a\naction = nope\n' \
	> "${TMP}/badact.cfg"
"${BIN}" -c "${TMP}/badact.cfg" >/dev/null 2>&1 && fail "bad action should fail"
printf '[xrdmoncollect]\nport = 9933\n[filter "x"]\naction = drop\n' > "${TMP}/nocond.cfg"
"${BIN}" -c "${TMP}/nocond.cfg" >/dev/null 2>&1 && fail "rule with no conditions should fail"

# 9. Two rules whose names differ only in case would silently share one set of
#    values (INIReader folds "<section>=<key>" but not the section list), so
#    they are rejected as duplicates.
printf '[xrdmoncollect]\nport = 9933\n[filter "x"]\nuser = a\n[filter "X"]\nuser = b\n' \
	> "${TMP}/dup.cfg"
"${BIN}" -c "${TMP}/dup.cfg" >/dev/null 2>&1 && fail "duplicate rule name should fail"

# 10. Indented keys hit inih's multi-line continuation: everything after the
#     first key is appended to that key's value. Report it instead of quietly
#     loading a rule that matches nothing.
printf '[xrdmoncollect]\nport = 9933\n[filter "x"]\n  user = daemon\n  authprot = sss\n' \
	> "${TMP}/indent.cfg"
out=$("${BIN}" -c "${TMP}/indent.cfg" 2>&1)
test $? -ne 0 || fail "indented filter keys should fail"
grep -q "column 1" <<<"${out}" || fail "expected the column-1 hint, got: ${out}"

# 11. A valid rule set loads, is reported at start-up, and the collector runs.
cat > "${TMP}/filter.cfg" <<EOF
[xrdmoncollect]
port = 9934
[filter "eos-internal"]
user     = daemon, root, ~^[0-9]+\$
authprot = sss
action   = tag
label    = internal
[filter "eos-proc"]
path   = /eos/*/proc/*
action = drop
EOF
"${BIN}" -c "${TMP}/filter.cfg" >/dev/null 2>"${TMP}/summary" &
pid=$!
sleep 1
kill -0 "${pid}" 2>/dev/null || fail "valid filter config should start"
kill "${pid}" 2>/dev/null; wait "${pid}" 2>/dev/null
# Dropping documents is not something to do silently: it is reported at start-up
# whether or not -v was given.
grep -q "2 filter rule(s) loaded (1 tag, 1 drop, 0 keep)" "${TMP}/summary" \
	|| fail "expected the rule summary, got: $(cat "${TMP}/summary")"

echo "ok"
