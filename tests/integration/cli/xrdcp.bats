#!/usr/bin/env bash

load ../functions.sh

setup_file() {
	setup_paths
	setup_client
}

@test "xrdcp comes from the build directory" {
	[[ "$(command -v xrdcp)" =~ ${BINARY_DIR} ]]
}

@test "xrdcp loads default plugin if set" {
	run -0 env XRD_LOGLEVEL=Debug XRD_PLUGIN=/dev/null xrdcp --version
	grep -cq 'Loading default plug-in from /dev/null' "${XRD_LOGFILE}"
}
