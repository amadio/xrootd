# Copyright (c) 2026 by the XRootD developers
# This file is part of the XRootD software suite and is distributed under the
# terms of the GNU Lesser General Public License, version 3 or later.
"""Native range reads, callback ownership and scoped asyncio cancellation."""

import asyncio
import gc
import sys
import threading
import uuid
import weakref

import pytest

from XRootD import client
from XRootD.client.flags import OpenFlags, QueryCode
from XRootD.client.responses import XRootDStatus
from env import SERVER_URL


@pytest.fixture(scope='module')
def remote_ranges():
    fs = client.FileSystem(SERVER_URL)
    status, reply = fs.query(QueryCode.CONFIG, 'readv_iov_max readv_ior_max')
    assert status.ok
    max_chunks, max_size = (int(value) for value in reply.split())
    # One logical range exceeds the server's per-chunk limit; many small
    # ranges also exceed its request count limit.
    size = max_size * 2 + 31
    pattern = bytes(bytearray(range(256)))
    payload = (pattern * ((size + 255) // 256))[:size]
    path = '/tmp/read-ranges-' + uuid.uuid4().hex
    url = SERVER_URL + path
    file = client.File()
    try:
        assert file.open(url, OpenFlags.DELETE)[0].ok
        assert file.write(payload)[0].ok
        assert file.close()[0].ok
        yield url, payload, max_chunks
    finally:
        if file.is_open():
            file.close()
        assert fs.rm(path)[0].ok


@pytest.fixture
def file(remote_ranges):
    opened = client.File()
    assert opened.open(remote_ranges[0], OpenFlags.READ)[0].ok
    try:
        yield opened
    finally:
        if opened.is_open():
            assert opened.close()[0].ok


def test_large_ordered_overlapping_and_empty_ranges(file, remote_ranges):
    _, payload, max_chunks = remote_ranges
    ranges = [(100, len(payload) - 100), (5, 19), (0, 0),
              (len(payload), 0), (10, 27)]
    status, result = file.read_ranges(ranges, timeout=10, parallel=2)
    assert status.ok
    assert result == [payload[start:start + size] for start, size in ranges]
    assert all(isinstance(item, bytes) for item in result)
    ranges = [(index, 1) for index in range(max_chunks + 1)]
    status, result = file.read_ranges(ranges, timeout=10, parallel=2)
    assert status.ok
    assert result == [payload[index:index + 1] for index, _ in ranges]
    status, result = file.read_ranges([])
    assert status.ok and result == []


def test_short_range_returns_error_without_partial_buffers(file,
                                                           remote_ranges):
    size = len(remote_ranges[1])
    status, result = file.read_ranges([(0, 4), (size - 2, 4)], timeout=10)
    assert not status.ok
    assert result is None


@pytest.mark.parametrize('chunks, kwargs, error', [
    ((), {}, TypeError),
    ([[0, 1]], {}, TypeError),
    ([(0,)], {}, TypeError),
    ([(True, 1)], {}, TypeError),
    ([(0, False)], {}, TypeError),
    ([(0.0, 1)], {}, TypeError),
    ([(-1, 1)], {}, OverflowError),
    ([(0, -1)], {}, OverflowError),
    ([(1 << 64, 0)], {}, OverflowError),
    ([((1 << 64) - 1, 1)], {}, OverflowError),
    ([(0, sys.maxsize + 1)], {}, OverflowError),
    ([], {'parallel': 0}, ValueError),
    ([], {'parallel': True}, TypeError),
    ([], {'parallel': -1}, OverflowError),
    ([], {'parallel': 65536}, OverflowError),
    ([], {'timeout': False}, TypeError),
    ([], {'timeout': -1}, OverflowError),
    ([], {'timeout': 65536}, OverflowError),
    ([], {'callback': False}, TypeError),
])
def test_invalid_ranges_do_not_submit(file, chunks, kwargs, error):
    with pytest.raises(error):
        file.read_ranges(chunks, **kwargs)
    assert file.read_ranges([(0, 1)])[0].ok


def test_closed_file_rejects_before_invoking_callback():
    calls = []
    file = client.File()
    with pytest.raises(ValueError, match='closed'):
        file.read_ranges([], callback=lambda *args: calls.append(args))
    assert not calls


def test_callback_pins_native_file_and_completes_once(remote_ranges):
    url, payload, _ = remote_ranges
    file = client.File()
    assert file.open(url, OpenFlags.READ)[0].ok
    native = file._File__file
    original_refs = sys.getrefcount(native)
    wrapper_ref = weakref.ref(file)
    entered, release, finished = (threading.Event() for _ in range(3))
    calls = []
    thread = threading.get_ident()

    def callback(status, result, hosts):
        calls.append((status, result, hosts, threading.get_ident()))
        entered.set()
        release.wait(10)
        finished.set()

    try:
        submitted = file.read_ranges([(0, len(payload))], timeout=10,
                                     callback=callback, parallel=2)
        assert submitted.ok
        # Event.wait releases the GIL, permitting the native callback to enter
        # Python. Keep it pending while checking the native File reference.
        assert entered.wait(10)
        assert sys.getrefcount(native) >= original_refs + 1
        del file
        gc.collect()
        assert wrapper_ref() is None
        del native
    finally:
        release.set()
        assert finished.wait(10)
    assert len(calls) == 1
    status, result, hosts, callback_thread = calls[0]
    assert status.ok and result == [payload]
    assert list(hosts) == []
    assert callback_thread != thread


def run_async(coroutine):
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coroutine)
    finally:
        loop.close()


@pytest.mark.skipif(sys.version_info < (3, 11),
                    reason='aio requires Python 3.11')
def test_aio_ranges_round_trip_and_native_errors(remote_ranges):
    from XRootD.client import aio
    from XRootD.client.responses import XRootDError

    async def run():
        url, payload, _ = remote_ranges
        file = await aio.File().open(url)
        try:
            ranges = [(9, len(payload) - 9), (0, 3), (0, 0)]
            result = await file.read_ranges(ranges, timeout=10, parallel=2)
            assert result == [payload[start:start + size]
                              for start, size in ranges]
            with pytest.raises(XRootDError):
                await file.read_ranges([(len(payload), 1)], timeout=10)
        finally:
            await file.close()

    run_async(run())


@pytest.mark.skipif(sys.version_info < (3, 11),
                    reason='aio requires Python 3.11')
@pytest.mark.parametrize('completion_ok', [True, False])
def test_aio_repeated_cancellation_waits_for_native_callback(completion_ok):
    from XRootD.client import aio

    async def run():
        callbacks = []
        finished = []

        class Native:
            def read_ranges(self, chunks, timeout, callback, parallel):
                assert chunks == [(0, 4)] and timeout == 10 and parallel == 2
                callbacks.append(callback)
                return XRootDStatus({'ok': True, 'code': 0, 'errno': 0,
                                     'message': 'submitted'})

        file = aio.File(native=Native())
        task = asyncio.ensure_future(file.read_ranges([(0, 4)], 10, 2))
        while not callbacks:
            await asyncio.sleep(0)
        task.cancel()
        await asyncio.sleep(0)
        task.cancel()
        await asyncio.sleep(0)
        assert not task.done()
        status = XRootDStatus({
            'ok': completion_ok,
            'code': 0 if completion_ok else XRootDStatus.errInternal,
            'errno': 0, 'message': 'complete',
        })

        def complete():
            callbacks[0](status, [b'data'] if completion_ok else None, [])
            finished.append(True)

        thread = threading.Thread(target=complete)
        thread.start()
        with pytest.raises(asyncio.CancelledError):
            await asyncio.wait_for(task, 2)
        thread.join(timeout=2)
        assert finished == [True]

    run_async(run())
