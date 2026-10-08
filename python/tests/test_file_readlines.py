"""Exercise line reads against real native local-file I/O, without a server."""
import json
import subprocess
import sys
from pathlib import Path

import pytest

from XRootD import client
from XRootD.client.flags import OpenFlags


@pytest.fixture
def opened_file(tmpdir):
    path = Path(str(tmpdir)) / 'lines'
    path.write_bytes(b'first\nsecond\nlast')
    file = client.File()
    status, _ = file.open(path.as_uri(), OpenFlags.READ)
    assert status.ok
    yield file, path
    file.close()


@pytest.mark.parametrize('offset', [1, 6, 13, 17, 100])
def test_readlines_explicit_offset_terminates(opened_file, offset):
    _, path = opened_file
    # A regression must time out instead of hanging the entire test runner.
    script = '''
import json, sys
from XRootD import client
with client.File() as file:
    assert file.open(sys.argv[1])[0].ok
    print(json.dumps(file.readlines(offset=int(sys.argv[2]), chunksize=2)))
'''
    result = subprocess.run([sys.executable, '-c', script, path.as_uri(),
                             str(offset)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                            universal_newlines=True,
                            timeout=10, check=True)
    expected = path.read_bytes()[offset:].decode().splitlines(True)
    assert json.loads(result.stdout) == expected


@pytest.mark.parametrize('chunksize', [0, 1, 2, 7, 65536, 1048576])
def test_readlines_uses_and_advances_cursor(opened_file, chunksize):
    file, _ = opened_file
    assert file.readline() == 'first\n'
    assert file.readlines(chunksize=chunksize) == ['second\n', 'last']
    assert file.readlines() == []


@pytest.mark.parametrize('size,chunksize', [(1, 1), (3, 2), (5, 2), (7, 3)])
def test_readline_never_exceeds_size(opened_file, size, chunksize):
    file, path = opened_file
    pieces = file.readlines(size=size, chunksize=chunksize)
    assert ''.join(pieces).encode() == path.read_bytes()
    assert all(len(piece.encode()) <= size for piece in pieces)


@pytest.mark.parametrize('argument', ['offset', 'size', 'chunksize'])
@pytest.mark.parametrize('value', [-1, 2 ** 64, 1.5, '2'])
def test_readlines_rejects_invalid_arguments(opened_file, argument, value):
    file, _ = opened_file
    with pytest.raises((TypeError, OverflowError, ValueError)):
        file.readlines(**{argument: value})


def test_readlines_preserves_unicode_error(opened_file):
    file, path = opened_file
    path.write_bytes(b'valid\n\xff\n')
    with pytest.raises(UnicodeDecodeError):
        file.readlines(chunksize=1)


def test_readlines_closed_file():
    with pytest.raises(ValueError, match='closed file'):
        client.File().readlines()


@pytest.mark.parametrize('method', ['readline', 'readlines'])
def test_line_reads_preserve_native_read_errors(tmpdir, method):
    path = Path(str(tmpdir)) / 'write-only'
    path.write_bytes(b'contents\n')
    with client.File() as file:
        assert file.open(path.as_uri(), OpenFlags.WRITE)[0].ok
        with pytest.raises(OSError):
            getattr(file, method)()
