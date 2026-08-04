#!/usr/bin/env bash

# End-to-end test for the XRootD fio I/O engine (src/XrdClFio, fio-xrootd.so).
# Runs a set of representative asynchronous workloads against the local server
# started by test.sh, exercising read/write, random/mixed patterns, data
# verification, the PgRead/PgWrite CRC path, VectorRead coalescing and fsync.

function test_fio() {
	local ENGINE="${BINARY_DIR}/lib/fio-xrootd.so"

	# Use the fio the engine was built against (its FIO_IOOPS_VERSION must
	# match); CMake passes it via $FIO, otherwise fall back to PATH.
	local FIO="${FIO:-fio}"
	require_commands "${FIO}"

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
	echo "fio:    $("${FIO}" --version)"
	echo

	# Sequential async write, then random read of the same file.
	assert "${FIO}" --name=write --ioengine="external:${ENGINE}" \
		--filename="${URL}/seq.dat" --rw=write --bs=1m --size=32m --iodepth=8

	assert "${FIO}" --name=randread --ioengine="external:${ENGINE}" \
		--filename="${URL}/seq.dat" --rw=randread --bs=64k --size=32m --iodepth=16

	# End-to-end data integrity through the asynchronous path: fio writes
	# checksummed blocks and reads them back to verify.
	assert "${FIO}" --name=verify --ioengine="external:${ENGINE}" \
		--filename="${URL}/verify.dat" --rw=write --bs=64k --size=16m \
		--iodepth=8 --verify=crc32c --do_verify=1 --verify_fatal=1

	# Mixed random read/write.
	assert "${FIO}" --name=randrw --ioengine="external:${ENGINE}" \
		--filename="${URL}/randrw.dat" --rw=randrw --rwmixread=70 \
		--bs=64k --size=16m --iodepth=8

	# PgRead/PgWrite with server-side CRC32C page checksums.
	assert "${FIO}" --name=pgwrite --ioengine="external:${ENGINE}" \
		--filename="${URL}/pgio.dat" --rw=write --bs=64k --size=16m \
		--iodepth=8 --xrd_pgio=1
	assert "${FIO}" --name=pgread --ioengine="external:${ENGINE}" \
		--filename="${URL}/pgio.dat" --rw=randread --bs=64k --size=16m \
		--iodepth=8 --xrd_pgio=1

	# VectorRead coalescing of queued reads.
	assert "${FIO}" --name=readv --ioengine="external:${ENGINE}" \
		--filename="${URL}/seq.dat" --rw=randread --bs=64k --size=32m \
		--iodepth=16 --xrd_readv=1

	# Periodic fsync during a write workload.
	assert "${FIO}" --name=fsync --ioengine="external:${ENGINE}" \
		--filename="${URL}/fsync.dat" --rw=write --bs=64k --size=16m \
		--iodepth=8 --fsync=8

	# Multiple files via filename_format + nrfiles: fio generates one URL per
	# file (f.<jobnum>.<filenum>) and splits the workload across them. Against a
	# cluster the redirector spreads these over data servers; here it exercises
	# the engine's per-file handling. Note filename_format is used verbatim, so
	# its colons are NOT backslash-escaped (unlike filename=).
	assert "${FIO}" --name=multiwrite --ioengine="external:${ENGINE}" \
		--filename_format="root://localhost:${XRD_PORT}//fiobench/f.\$jobnum.\$filenum" \
		--nrfiles=4 --rw=write --bs=256k --size=16m --iodepth=8

	assert "${FIO}" --name=multiread --ioengine="external:${ENGINE}" \
		--filename_format="root://localhost:${XRD_PORT}//fiobench/f.\$jobnum.\$filenum" \
		--nrfiles=4 --rw=randread --bs=64k --size=16m --iodepth=16

	# Re-read the same files with backslash-escaped colons in filename_format:
	# fio stores the format verbatim, so this reaches the engine with literal
	# '\:' sequences. The engine normalizes them, so both forms resolve to the
	# same URLs (guards the xrd_url() escaping-agnostic path).
	assert "${FIO}" --name=multiread_esc --ioengine="external:${ENGINE}" \
		--filename_format="root\\://localhost\\:${XRD_PORT}//fiobench/f.\$jobnum.\$filenum" \
		--nrfiles=4 --rw=randread --bs=64k --size=16m --iodepth=16
}
