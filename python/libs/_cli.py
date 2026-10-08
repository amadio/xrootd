"""Launch the native clients shipped alongside the Python bindings."""

import os
import sys


def _payload_dir():
    return os.path.join(os.path.dirname(os.path.dirname(__file__)), 'pyxrootd')


def _configure_plugins():
    directory = _payload_dir()
    if os.path.isfile(os.path.join(directory, 'http.conf')):
        os.environ.setdefault('XRD_PLUGINCONFDIR', directory)


def _exec(command):
    # Resolve from this installation, never from PATH, so all clients use the
    # matching bundled libraries. exec preserves signals and native exit codes.
    _configure_plugins()
    executable = os.path.join(_payload_dir(), command)
    os.execv(executable, [command] + sys.argv[1:])


def xrdfs():
    _exec('xrdfs')


def xrdcp():
    _exec('xrdcp')


def xrdcopy():
    _exec('xrdcp')


def xrdtoken():
    _exec('xrdtoken')
