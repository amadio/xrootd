"""Native HTTP regression tests using disposable stdlib loopback servers."""

import contextlib
import http.server
import os
from pathlib import Path
import socketserver
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from urllib.parse import urlsplit


BIN, LIB, DRIVER = sys.argv[1:4]
sys.argv[1:] = []


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, body=b"", **headers):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for key, value in headers.items():
            self.send_header(key.replace("_", "-"), value)
        self.end_headers()
        self.close_connection = True
        if self.command != "HEAD" and body:
            self.wfile.write(body)

    def handle_expect_100(self):
        if self.server.redirect and not self.server.consume_redirect:
            self.reply(307, Location=self.server.redirect + self.path)
            return False
        return super().handle_expect_100()

    def do_OPTIONS(self):
        self.reply(200, Allow="HEAD,GET,PUT,PROPFIND,OPTIONS")

    def do_HEAD(self):
        self.reply(404)

    def do_PROPFIND(self):
        depth = self.headers.get("Depth")
        self.server.depths.append(depth)
        if self.server.redirect:
            self.reply(307, Location=self.server.redirect + self.path)
            return
        if urlsplit(self.path).path != "/dir":
            self.reply(404)
            return
        if depth not in ("0", "1"):
            self.reply(501, b"NOT_IMPLEMENTED")
            return
        href = urlsplit(self.path).path
        # EOS uses lower-case d:collection and omits collection content length.
        body = ("<d:multistatus xmlns:d='DAV:'><d:response>"
                "<d:href>{}</d:href><d:propstat><d:prop>"
                "<d:resourcetype><d:collection/></d:resourcetype>"
                "</d:prop><d:status>HTTP/1.1 200 OK</d:status>"
                "</d:propstat></d:response></d:multistatus>").format(href)
        self.reply(207, body.encode(), Content_Type="application/xml")

    def do_PUT(self):
        if self.server.redirect:
            if self.server.consume_redirect:
                self.rfile.read(int(self.headers.get("Content-Length", 0)))
            self.reply(307, Location=self.server.redirect + self.path)
            return
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            chunks = []
            while True:
                size = int(self.rfile.readline().split(b";")[0], 16)
                if not size:
                    self.rfile.readline()
                    break
                chunks.append(self.rfile.read(size))
                self.rfile.read(2)
            data = b"".join(chunks)
        else:
            data = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        time.sleep(self.server.delay)
        if self.server.reject:
            self.reply(500, b"upload rejected after receiving body")
        else:
            self.server.files[urlsplit(self.path).path] = data
            self.reply(201)


@contextlib.contextmanager
def serve(redirect=None, delay=0, reject=False, consume_redirect=False,
          tls=None):
    server = Server(("127.0.0.1", 0), Handler)
    server.files, server.depths = {}, []
    server.redirect, server.delay, server.reject = redirect, delay, reject
    server.consume_redirect = consume_redirect
    if tls:
        server.socket = tls.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        scheme = "https" if tls else "http"
        yield server, "{}://127.0.0.1:{}".format(scheme, server.server_port)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


class NativeHTTPTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        (self.directory / "http.conf").write_text(
            "url = http://*;https://*\nlib = {}\nenable = true\n".format(LIB))
        self.environment = {
            "PATH": os.environ["PATH"],
            "HOME": str(self.directory),
            "XRD_PLUGINCONFDIR": str(self.directory),
            "XRD_HTTPDISABLEX509": "1",
            "XRD_CONNECTIONWINDOW": "1",
            "XRD_CONNECTIONRETRY": "0",
            "XRD_REQUESTTIMEOUT": "5",
            "XRD_STREAMTIMEOUT": "5",
            "XRD_CPCHUNKSIZE": str(4 * 1024 * 1024),
        }

    def run_client(self, executable, *args):
        return subprocess.run(
            [executable] + list(args), stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=self.environment, timeout=15)

    def copy(self, url, payload):
        source = self.directory / "source"
        source.write_bytes(payload)
        return self.run_client(str(Path(BIN) / "xrdcp"), "--nopbar",
                               str(source), url + "/upload")

    def test_copy_sizes_and_redirects(self):
        sizes = (0, 65536, 1048576, 4194303, 4194304, 4194305, 16777216)
        for redirect in (False, True):
            with serve() as (destination, url):
                with serve(redirect=url) as (_, redirect_url):
                    for size in sizes:
                        with self.subTest(size=size, redirect=redirect):
                            payload = (bytes(range(256)) * (size // 256 + 1))
                            payload = payload[:size]
                            destination.files.clear()
                            target = redirect_url if redirect else url
                            result = self.copy(target, payload)
                            self.assertEqual(result.returncode, 0,
                                             result.stderr.decode())
                            self.assertEqual(destination.files.get("/upload"),
                                             payload)

    def test_wait_for_delayed_final_response(self):
        with serve(delay=0.3) as (server, url):
            start = time.monotonic()
            result = self.copy(url, b"payload")
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertGreaterEqual(time.monotonic() - start, 0.3)
            self.assertEqual(server.files["/upload"], b"payload")

    def test_replay_consumed_first_write_after_redirect(self):
        with serve() as (server, url):
            with serve(redirect=url, consume_redirect=True) as (_, redirect):
                payload = b"a" * 65536
                result = self.copy(redirect, payload)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertEqual(server.files["/upload"], payload)

    def test_propagate_final_rejection(self):
        with serve(reject=True, delay=0.1) as (server, url):
            for payload in (b"", b"payload", b"a" * 65536):
                with self.subTest(size=len(payload)):
                    result = self.copy(url, payload)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertNotIn("/upload", server.files)

    def test_stat_and_listing_send_depth_after_options(self):
        with serve() as (server, url):
            for command in (("stat", "/dir"), ("ls", "-l", "/dir")):
                result = self.run_client(str(Path(BIN) / "xrdfs"), url,
                                         *command)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertIn("0", server.depths)
            self.assertIn("1", server.depths)
            self.assertNotIn(None, server.depths)

    def test_stat_depth_survives_redirect(self):
        with serve() as (server, url):
            with serve(redirect=url) as (redirect, redirect_url):
                result = self.run_client(
                    str(Path(BIN) / "xrdfs"), redirect_url, "stat", "/dir")
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertEqual(server.depths, ["0"])
                self.assertEqual(redirect.depths, ["0"])

    def test_owning_buffer_writes(self):
        for mode in ("known", "unknown"):
            with serve(delay=0.1) as (server, url):
                result = self.run_client(DRIVER, url + "/upload", mode)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertEqual(server.files.get("/upload"),
                                 b"a" * 65536 + b"b" * 65536)

    @unittest.skipUnless(shutil.which("openssl"),
                         "openssl is required for TLS")
    def test_tls_trust_and_explicit_untrusted_directory(self):
        cert, key = self.directory / "cert.pem", self.directory / "key.pem"
        config = self.directory / "openssl.conf"
        config.write_text(
            "[req]\ndistinguished_name=dn\nx509_extensions=ext\n"
            "[dn]\n[ext]\nsubjectAltName=IP:127.0.0.1\n"
            "basicConstraints=critical,CA:TRUE\n")
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-days", "1", "-subj", "/CN=loopback-test", "-config",
            str(config), "-keyout", str(key), "-out", str(cert),
        ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        hashed = subprocess.check_output([
            "openssl", "x509", "-in", str(cert), "-noout", "-hash",
        ]).decode().strip()
        ca_dir = self.directory / "ca"
        ca_dir.mkdir()
        (ca_dir / (hashed + ".0")).symlink_to(cert)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(str(cert), str(key))
        with serve(tls=tls) as (server, url):
            self.environment["X509_CERT_DIR"] = str(ca_dir)
            result = self.copy(url, b"trusted")
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertEqual(server.files["/upload"], b"trusted")
            server.files.clear()
            untrusted = self.directory / "untrusted"
            untrusted.mkdir()
            self.environment["X509_CERT_DIR"] = str(untrusted)
            result = self.copy(url, b"untrusted")
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("/upload", server.files)


if __name__ == "__main__":
    unittest.main()
