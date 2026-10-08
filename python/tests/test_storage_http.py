"""Exercise the native HTTP plugin against an isolated loopback fixture."""

import json
import os
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from threading import Thread

import pytest

try:
  from http.server import ThreadingHTTPServer
except ImportError:  # Python 3.6 / EL8
  from socketserver import ThreadingMixIn

  class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True


@pytest.fixture
def webdav(tmp_path):
  binary = os.environ.get('XROOTD')
  libraries = list((Path(binary).resolve().parent.parent / 'lib').glob(
    'libXrdClHttp-*.so')) if binary else []
  if not libraries:
    pytest.skip('requires a built native HTTP client plugin')
  config = tmp_path / 'plugins'
  config.mkdir()
  (config / 'http.conf').write_text(
    'url = http://*\nlib = %s\nenable = true\n' % libraries[0])
  requests = []

  class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
      pass

    def reply(self, status=200, body=b'', **headers):
      requests.append((self.command, self.path,
                       self.headers.get('Authorization')))
      self.send_response(status)
      self.send_header('Content-Length', str(len(body)))
      for key, value in headers.items():
        self.send_header(key, value)
      self.end_headers()
      if self.command != 'HEAD':
        self.wfile.write(body)

    def do_OPTIONS(self):
      self.reply(Allow='OPTIONS, HEAD, GET, PROPFIND, MKCOL, MOVE, DELETE')

    def do_HEAD(self):
      self.reply(body=b'data', Digest='adler32=12345678')

    def do_PROPFIND(self):
      self.rfile.read(int(self.headers.get('Content-Length', 0)))
      if self.path.startswith('/missing'):
        self.reply(404)
        return
      if self.path.startswith('/denied'):
        self.reply(403)
        return
      self.reply(207, b'''<d:multistatus xmlns:d="DAV:"><d:response>
        <d:href>/file</d:href><d:propstat><d:prop>
        <d:getcontentlength>4</d:getcontentlength><d:resourcetype/>
        <d:quota-available-bytes>75</d:quota-available-bytes>
        <d:quota-used-bytes>25</d:quota-used-bytes>
        </d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>
        </d:response></d:multistatus>''')

    def do_GET(self):
      if self.path.startswith('/redirect'):
        self.reply(302, Location='/file')
        return
      if self.headers.get('Range'):
        self.reply(206, b'data', **{'Content-Range': 'bytes 0-3/4'})
      else:
        self.reply(body=b'data')

    def do_PUT(self):
      body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
      assert body == b'data'
      self.reply(201)

    def do_MKCOL(self):
      self.reply(201)

    def do_MOVE(self):
      self.reply(201)

    def do_DELETE(self):
      self.reply(207 if self.path.startswith('/partial') else 204)

  server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
  thread = Thread(target=server.serve_forever, daemon=True)
  thread.start()
  environment = {'XRD_PLUGINCONFDIR': str(config)}
  try:
    yield 'http://127.0.0.1:%d' % server.server_port, environment, requests
  finally:
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)


def run_client(webdav, code):
  endpoint, environment, _ = webdav
  result = subprocess.run([sys.executable, '-c', code, endpoint],
                          env=dict(os.environ, **environment), universal_newlines=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=30)
  assert result.returncode == 0, result.stderr
  return json.loads(result.stdout)


def test_native_http_contexts_and_queries(webdav):
  result = run_client(webdav, '''
import json, sys
from concurrent.futures import ThreadPoolExecutor
from XRootD.client import AuthContext, StorageClient
from XRootD.client.env import EnvPutString
EnvPutString('HttpHeaders', 'Authorization: Bearer ambient')
with AuthContext.bearer(token='first') as a, AuthContext.bearer(token='second') as b:
    with StorageClient(auth=a) as first, StorageClient(auth=b) as second:
        url = sys.argv[1] + '/file?signature=opaque+value==&empty='
        with ThreadPoolExecutor(max_workers=2) as pool:
            sizes = list(pool.map(lambda c: c.stat(url).size, (first, second)))
with StorageClient(auth=AuthContext.anonymous()) as anonymous:
    sizes.append(anonymous.stat(sys.argv[1] + '/file').size)
print(json.dumps(sizes))
''')
  assert result == [4, 4, 4]
  requests = webdav[2]
  assert {'Bearer first', 'Bearer second', None} <= {
    request[2] for request in requests}
  assert all('xrdcl.' not in request[1] for request in requests)
  assert any('signature=opaque+value==&empty=' in r[1] for r in requests)
  assert all(r[2] != 'Bearer ambient' for r in requests)


def test_native_webdav_namespace_quota_and_partial_delete(webdav):
  result = run_client(webdav, '''
import json, sys
from XRootD.client import AuthContext, StorageClient, XRootDOperationError
with StorageClient(auth=AuthContext.anonymous()) as client:
    client.move(sys.argv[1] + '/file', sys.argv[1] + '/new/parent/file')
    usage = client.space(sys.argv[1] + '/file')
    client.delete(sys.argv[1] + '/file')
    try:
        client.delete(sys.argv[1] + '/partial')
    except XRootDOperationError:
        partial = True
    else:
        partial = False
print(json.dumps([usage, partial]))
''')
  assert result == [{'total': 100, 'free': 75, 'used': 25}, True]
  methods = [r[0] for r in webdav[2]]
  assert methods.index('MKCOL') < methods.index('MOVE')


def test_native_http_metadata_and_error_contract(webdav):
  result = run_client(webdav, '''
import json, sys
from XRootD.client import AuthContext, StorageClient, XRootDAuthorizationError
with StorageClient(auth=AuthContext.anonymous()) as client:
    info = client.info(sys.argv[1] + '/file',
                       checksum_algorithms=('adler32',), require_checksum=True)
    missing = client.exists(sys.argv[1] + '/missing')
    try:
        client.exists(sys.argv[1] + '/denied')
    except XRootDAuthorizationError:
        denied = True
    else:
        denied = False
print(json.dumps([info.size, info.checksum.algorithm, info.checksum.value,
                  missing, denied]))
''')
  assert result == [4, 'adler32', '12345678', False, True]


def test_native_http_transfers_keep_context_after_redirect(webdav, tmp_path):
  endpoint, environment, requests = webdav
  target = tmp_path / 'download'
  code = """
import json, sys
from XRootD.client import AuthContext, StorageClient
with AuthContext.bearer(token='transfer') as auth, StorageClient(auth=auth) as client:
    client.get(sys.argv[1] + '/redirect', sys.argv[2], force=True)
    client.put(sys.argv[2], sys.argv[1] + '/upload', force=True,
               create_parents=False)
    query = '?signature=opaque+value==&empty=&repeat=one&repeat=two&bare&escaped=%2f%2F'
    client.put(sys.argv[2], sys.argv[1] + '/upload' + query, force=True,
               create_parents=False)
    client.get(sys.argv[1] + '/file' + query, sys.argv[2], force=True)
print(json.dumps(True))
"""
  result = subprocess.run([sys.executable, '-c', code, endpoint, str(target)],
                          env=dict(os.environ, **environment), universal_newlines=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
  assert result.returncode == 0, result.stderr
  assert target.read_bytes() == b'data'
  transfers = [r for r in requests if r[0] in ('GET', 'PUT')]
  assert transfers
  assert all(r[2] == 'Bearer transfer' for r in transfers)
  assert all(r[2] == 'Bearer transfer' for r in requests if r[0] == 'OPTIONS')
  assert all('xrdcl.' not in r[1] for r in transfers)
  query = '?signature=opaque+value==&empty=&repeat=one&repeat=two&bare&escaped=%2f%2F'
  assert any(r[0] == 'PUT' and r[1] == '/upload' + query for r in transfers)
  assert any(r[0] == 'GET' and r[1] == '/file' + query for r in transfers)
