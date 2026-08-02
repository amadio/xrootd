#!/usr/bin/env bash

# End-to-end test for the XRootD fio I/O engine (src/XrdClFio, fio-xrootd.so).
# Runs a set of representative asynchronous workloads against the local server
# started by test.sh, exercising read/write, random/mixed patterns, data
# verification, the PgRead/PgWrite CRC path, VectorRead coalescing and fsync.

function setup_fio() {
	require_commands fio
}

function test_fio() {
	local ENGINE="${BINARY_DIR}/lib/fio-xrootd.so"

	if [[ ! -f "${ENGINE}" ]]; then
		error "fio engine not found: ${ENGINE} (build with -DENABLE_FIO_ENGINE=ON)"
	fi

	# The engine dlopens libXrdCl from the build tree.
	export LD_LIBRARY_PATH="${BINARY_DIR}/lib:${LD_LIBRARY_PATH}"

	# fio splits filenames on ':', so the colons in the root:// URL must be
	# escaped with a backslash. MakePath (on by default) creates the subdir.
	local URL="root\\://localhost\\:${XRD_PORT}//fiobench"

	echo
	echo "engine: ${ENGINE}"
	echo "fio:    $(fio --version)"
	echo

	# Sequential async write, then random read of the same file.
	assert fio --name=write --ioengine="external:${ENGINE}" \
		--filename="${URL}/seq.dat" --rw=write --bs=1m --size=32m --iodepth=8

	assert fio --name=randread --ioengine="external:${ENGINE}" \
		--filename="${URL}/seq.dat" --rw=randread --bs=64k --size=32m --iodepth=16

	# End-to-end data integrity through the asynchronous path: fio writes
	# checksummed blocks and reads them back to verify.
	assert fio --name=verify --ioengine="external:${ENGINE}" \
		--filename="${URL}/verify.dat" --rw=write --bs=64k --size=16m \
		--iodepth=8 --verify=crc32c --do_verify=1 --verify_fatal=1

	# Mixed random read/write.
	assert fio --name=randrw --ioengine="external:${ENGINE}" \
		--filename="${URL}/randrw.dat" --rw=randrw --rwmixread=70 \
		--bs=64k --size=16m --iodepth=8

	# PgRead/PgWrite with server-side CRC32C page checksums.
	assert fio --name=pgwrite --ioengine="external:${ENGINE}" \
		--filename="${URL}/pgio.dat" --rw=write --bs=64k --size=16m \
		--iodepth=8 --xrd_pgio=1
	assert fio --name=pgread --ioengine="external:${ENGINE}" \
		--filename="${URL}/pgio.dat" --rw=randread --bs=64k --size=16m \
		--iodepth=8 --xrd_pgio=1

	# VectorRead coalescing of queued reads.
	assert fio --name=readv --ioengine="external:${ENGINE}" \
		--filename="${URL}/seq.dat" --rw=randread --bs=64k --size=32m \
		--iodepth=16 --xrd_readv=1

	# Periodic fsync during a write workload.
	assert fio --name=fsync --ioengine="external:${ENGINE}" \
		--filename="${URL}/fsync.dat" --rw=write --bs=64k --size=16m \
		--iodepth=8 --fsync=8
}
