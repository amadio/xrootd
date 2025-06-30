#!/usr/bin/env bash

function setup_posix() {
	require_commands cat mkdir rmdir truncate
}

function test_posix() {
	# Enable XrdPosix by setting LD_LIBRARY_PATH and LD_PRELOAD

	LD_LIBRARY_PATH=${BINARY_DIR}/lib
	LD_PRELOAD=${BINARY_DIR}/lib/libXrdPosixPreload.so

	# Make sure all symbols are actually resolved correctly

	export XRDPOSIX_REPORT=1

	MISSING_SYMBOLS=$(env LD_PRELOAD=${LD_PRELOAD} true 2>&1)

        if grep -q 'Unable to resolve' <<< "${MISSING_SYMBOLS}"; then
                echo "XrdPosix cannot resolve some symbols:"
                echo "${MISSING_SYMBOLS}"
                exit 1
        fi

	export LD_LIBRARY_PATH LD_PRELOAD

	# Create some sample text files

	echo "This is a local text file." >| "${LOCAL_DIR}"/local.txt
	echo "This is a remote text file." >| "${REMOTE_DIR}"/remote.txt

	# Check that cat with the local files still works

	assert cat "${LOCAL_DIR}"/local.txt
	assert cat "${REMOTE_DIR}"/remote.txt

	# Check that cat with remote file URL works

	assert cat "${HOST}"/remote.txt

	# Use XrdPosix via virtual mount point

	export XROOTD_VMP="${HOST#root://}:/xrootd/=/"

	assert cat /xrootd/remote.txt

	# Check that we can truncate the file

	assert truncate --size=0 "${HOST}"/remote.txt
	assert cat "${HOST}"/remote.txt

	# Check that we can create and remove a directory

	assert mkdir "${HOST}"/tmp
	assert ls -l "${REMOTE_DIR}"
	assert test -d "${REMOTE_DIR}"/tmp

	assert rmdir "${HOST}"/tmp
	assert ls -l "${REMOTE_DIR}"
	assert test ! -d "${REMOTE_DIR}"/tmp
}
