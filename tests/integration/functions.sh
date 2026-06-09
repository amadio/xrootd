#!/usr/bin/env bash

bats_require_minimum_version 1.5.0

# This file helps prepare common test environments.

function error() {
	echo "error: $*" >&2; exit 1;
}

function assert() {
	echo "$@"; "$@" || error "command \"$*\" failed";
}

# $1 is expected_value $2 is received value $3 is the error message
function assert_eq() {
	[[ "$1" == "$2" ]] || error "$3: expected $1 but received $2"
}

# Ensure two returned values are not equal to each other
# $1 is expected_value $2 is received value $3 is the error message
function assert_ne() {
	[[ "$1" != "$2" ]] || error "$3: expected $1 to not be equal to $2"
}

function assert_failure() {
	echo "$@"; "$@" && error "command \"$*\" did not fail";
}

function require_commands() {
	for PROG in "$@"; do
		if [[ ! -x "$(command -v "${PROG}")" ]]; then
			error "'${PROG}': command not found"
		fi
	done
}

function start_server() {
	CONFIG="${1:-"${BATS_TEST_DIRNAME}/xrootd.conf"}"

	test -f "${CONFIG}" || return 1

	CONFIG_BASE="$(basename "${1:-xrootd.conf}")"
	SERVER="${CONFIG_BASE%.conf}"
	SERVER="${SERVER%-*}"
	NAME="${CONFIG_BASE%.conf}"
	NAME="${NAME#*-}"

	run -0 cconfig -x "${SERVER}" -c "${CONFIG}"
	run -0 "${SERVER}" -b -a "${PWD}" -w "${PWD}" -c "${CONFIG}" -n "${NAME}" -l "${SERVER}.log" -s "${SERVER}.pid"
}

function start_servers()
{
	find "${BATS_TEST_DIRNAME}" -type f -name '*-*.conf' -print0 |
	while IFS= read -r -d '' CONFIG; do
		start_server "${CONFIG}"
	done
}

function setup_paths() {
	# Source directory is the root directory of the repository.
	: "${SOURCE_DIR:?"Error: cannot determine source directory"}"

	# Binary directory is the build directory configured with CMake.
	: "${BINARY_DIR:?"Error: cannot determine binary directory"}"

	# Ensure we test the binaries we just built, by prepending the
	# build directory's bin/ subdirectory to our PATH, and checking
	# that XRootD commands really come from there.

	PATH="${SOURCE_DIR}/scripts:${BINARY_DIR}/bin:${PATH}"

	for PROG in cconfig cmsd xrootd xrdcp xrdfs xrdadler32; do
		if [[ ! "$(command -v "${PROG}")" =~ ${BINARY_DIR} ]]; then
			error "'${PROG}': not used from build directory"
		fi
	done

	export PATH
}

function setup_client() {
	# Clean up the surrounding environment

	unset XRD_CLCONFDIR
	unset XRD_CLCONFFILE

	# Reduce timeouts to catch errors quickly and prevent the test
	# suite from getting stuck waiting for timeouts while running.

	: "${XRD_REQUESTTIMEOUT:=15}"
	: "${XRD_STREAMTIMEOUT:=10}"
	: "${XRD_TIMEOUTRESOLUTION:=1}"

	export XRD_REQUESTTIMEOUT XRD_STREAMTIMEOUT XRD_TIMEOUTRESOLUTION

	# Default client log level. This must be either Debug or Dump.

	: "${XRD_LOGLEVEL:=Debug}"

	export XRD_LOGLEVEL

}

function setup_file() {
	setup_paths
	setup_client
	pushd "${BATS_FILE_TMPDIR}" || return 1
	NAME="$(basename "${BATS_TEST_FILENAME%.bats}")"
	if [[ $(type -t "setup_${NAME}") == "function" ]]; then
                "setup_${NAME}"
        fi
	start_servers
	popd || return 1
}

function teardown_file() {
        find "${BATS_FILE_TMPDIR}" -type f -name '*.pid' -print0 | while IFS= read -r -d '' PIDFILE; do
                local PID
                test -s "${PIDFILE}"
                PID="$(ps -o pid= "$(cat "${PIDFILE}")" || true)"
                if test -n "${PID}"; then
                        echo "Found running pid $PID in $PIDFILE"
                        kill -s TERM "${PID}"
                fi
                rm "${PIDFILE}"
        done
        find "${BATS_FILE_TMPDIR}" -type f -name '*.log' -print0 | while IFS= read -r -d '' LOGFILE; do
		echo "=> ${LOGFILE} <="
		tail -n "${LOGLINES:-50}" "${LOGFILE}"
                rm "${LOGFILE}"
        done
}

function setup() {
	pushd "${BATS_TEST_TMPDIR}" || return 1
	export XRD_LOGFILE="${BATS_TEST_TMPDIR}/client.log"
}

function teardown() {
	tail -n "${LOGLINES:-50}" "${XRD_LOGFILE}"
	popd || return 1
}
