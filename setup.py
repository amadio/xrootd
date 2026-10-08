import os
import shutil
import subprocess
import sys

from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
from subprocess import check_call, check_output

try:
    from shutil import which
except ImportError:
    from distutils.spawn import find_executable as which

cmake = which("cmake3") or which("cmake")


def get_cmake_args():
    args = os.getenv('CMAKE_ARGS')

    if not args:
        return []

    from shlex import split
    return split(args)


def get_version():
    try:
        with open('VERSION') as f:
            version = f.read().strip()

        if version.startswith('$'):
            output = check_output(['git', 'describe', '--match', 'v*'])
            version = output.decode().strip()
    except (OSError, ValueError, subprocess.CalledProcessError):
        version = None

    if version is None:
        from datetime import date
        version = '5.9-rc' + date.today().strftime("%Y%m%d")

    if version.startswith('v'):
        version = version[1:]

    # Sanitize version to conform to PEP 440
    # https://www.python.org/dev/peps/pep-0440
    version = version.replace('-rc', 'rc')
    version = version.replace('-g', '+git.')
    version = version.replace('-', '.post', 1)
    version = version.replace('-', '.')

    return version


class CMakeExtension(Extension):
    def __init__(self, name, src='.', sources=[], **kwa):
        Extension.__init__(self, name, sources=sources, **kwa)
        self.src = os.path.abspath(src)


class CMakeBuild(build_ext):
    def build_extensions(self):
        if cmake is None:
            raise RuntimeError('Cannot find CMake executable')

        for ext in self.extensions:
            extension_path = self.get_ext_fullpath(ext.name)
            extdir = os.path.abspath(os.path.dirname(extension_path))

            # Use $ORIGIN RPATH to ensure that the client library can load
            # libXrdCl which will be installed in the same directory. Build
            # with install RPATH because libraries are installed by Python/pip
            # not CMake, so CMake cannot fix the install RPATH later on.

            cmake_args = [
                '-DPython_EXECUTABLE={}'.format(sys.executable),
                '-DCMAKE_BUILD_WITH_INSTALL_RPATH=TRUE',
                '-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY={}/{}'.format(
                    self.build_temp, ext.name),
                '-DPYXROOTD_PAYLOAD_DIR={}'.format(
                    os.path.abspath(os.path.join(self.build_temp, 'payload'))),
                '-DENABLE_PYTHON=1', '-DENABLE_XRDCL=1',
                '-DXRDCL_ONLY=1', '-DPYPI_BUILD=1', '-DENABLE_READLINE=0'
            ]

            if sys.platform == 'darwin':
                cmake_args += ['-DCMAKE_INSTALL_RPATH=@loader_path']
            else:
                cmake_args += ['-DCMAKE_INSTALL_RPATH=$ORIGIN']

            cmake_args += get_cmake_args()

            if not os.path.exists(self.build_temp):
                os.makedirs(self.build_temp)

            check_call([cmake, ext.src, '-B', self.build_temp] + cmake_args)
            check_call([cmake, '--build', self.build_temp,
                        '--target', 'PyXRootDClientPayload'])
            destdir = os.path.join(extdir, ext.name)
            if os.path.isdir(destdir):
                shutil.rmtree(destdir)
            # Recreate directly; distutils caches previously created paths.
            os.makedirs(destdir)
            self.copy_file('python/src/__init__.py', destdir)
            payload = os.path.join(self.build_temp, 'payload')
            for entry in sorted(os.listdir(payload)):
                self.copy_file(os.path.join(payload, entry), destdir)


# Keep the token workflow implementation in its own topic. Its native source
# is absent until that topic lands, so do not advertise a non-working command.
client_commands = ['xrdfs', 'xrdcp', 'xrdcopy']
if os.path.isfile('src/XrdApps/XrdToken.cc'):
    client_commands.append('xrdtoken')

version = get_version()

setup(name='xrootd',
      version=version,
      description='eXtended ROOT daemon',
      author='XRootD Developers',
      author_email='xrootd-dev@slac.stanford.edu',
      url='https://xrootd.org',
      download_url=('https://github.com/xrootd/xrootd/archive/v%s.tar.gz'
                    % version),
      keywords=['XRootD', 'network filesystem'],
      license='LGPL-3.0-or-later',
      long_description=open('README.md').read(),
      long_description_content_type='text/markdown',
      packages=['XRootD', 'XRootD.client', 'pyxrootd'],
      package_data={
          'XRootD.client': ['py.typed'],
      },
      package_dir={
          'pyxrootd': 'python/src',
          'XRootD': 'python/libs',
          'XRootD/client': 'python/libs/client',
      },
      ext_modules=[CMakeExtension('pyxrootd')],
      cmdclass={'build_ext': CMakeBuild},
      entry_points={'console_scripts': [
          '{}=XRootD._cli:{}'.format(name, name)
          for name in client_commands
      ]},
      python_requires='>=3.6',
      zip_safe=False,
      classifiers=[
          "Intended Audience :: Information Technology",
          "Intended Audience :: Science/Research",
          "Operating System :: MacOS",
          "Operating System :: POSIX :: Linux",
          "Operating System :: Unix",
          "Programming Language :: C++",
          "Programming Language :: Python",
      ]
      )
