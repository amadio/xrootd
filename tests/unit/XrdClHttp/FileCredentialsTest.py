#!/usr/bin/env python3
"""Keep per-file authentication policy through HEAD, GET, PUT and redirects."""

import argparse
import contextlib
import http.server
import os
from pathlib import Path
import socketserver
import subprocess
import tempfile
import threading
from urllib.parse import quote, urlsplit

PAYLOAD = b"file-credentials\0\xff" * 8192


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def reply(self, status, data=b"", headers=None):
        self.send_response(status)
        self.send_header("Content-Length", str(len(data)))
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        if self.command != "HEAD":
            with contextlib.suppress(BrokenPipeError, ConnectionResetError):
                self.wfile.write(data)

    def parse_request(self):
        if not super().parse_request():
            return False
        if "xrdcl.http." in self.path:
            self.server.leaked = True
        if self.command != "OPTIONS" and (
            self.headers.get("Authorization") != self.server.expected
        ):
            self.server.denied = True
            self.close_connection = True
            self.reply(403)
            return False
        return True

    def do_OPTIONS(self):
        self.reply(200)

    def do_HEAD(self):
        if urlsplit(self.path).path == "/redirect":
            self.reply(307, headers={"Location": "/file?signature=a%2Bb"})
        else:
            self.reply(200, self.server.payload)

    def do_GET(self):
        payload = self.server.payload
        bounds = self.headers.get("Range")
        if bounds:
            first, last = bounds[len("bytes="):].split("-")
            start = int(first)
            end = min(
                int(last) if last else len(payload) - 1, len(payload) - 1
            )
            self.reply(
                206,
                payload[start:end + 1],
                {
                    "Content-Range": "bytes {}-{}/{}".format(
                        start, end, len(payload)
                    )
                },
            )
        else:
            self.reply(200, payload)

    def do_PUT(self):
        if self.headers.get("Transfer-Encoding") == "chunked":
            chunks = []
            while True:
                size = int(self.rfile.readline().strip(), 16)
                if not size:
                    self.rfile.readline()
                    break
                chunks.append(self.rfile.read(size))
                self.rfile.read(2)
            self.server.payload = b"".join(chunks)
        else:
            self.server.payload = self.rfile.read(
                int(self.headers.get("Content-Length", "0"))
            )
        self.reply(201)


@contextlib.contextmanager
def endpoint():
    server = Server(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xrdcp", required=True)
    parser.add_argument("--plugin", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="xrd-file-credentials-") as tmp:
        root = Path(tmp)
        (root / "http.conf").write_text(
            "url = http://*\nlib = {}\nenable = true\n".format(args.plugin)
        )
        environment = dict(os.environ)
        for key in tuple(environment):
            if key.startswith("X509_") or key in (
                "BEARER_TOKEN",
                "BEARER_TOKEN_FILE",
                "GFAL_AUTHZ_TOKEN",
            ):
                environment.pop(key)
        environment.update(
            XRD_PLUGINCONFDIR=tmp,
            X509_USER_PROXY="/dev/null/proxy",
            BEARER_TOKEN="wrong-ambient-fixture-token",
        )
        with endpoint() as source, endpoint() as target:
            for redirect in (False, True):
                for src_auth, dst_auth in (
                    (True, True),
                    (False, True),
                    (True, False),
                    (False, False),
                ):
                    urls = []
                    for server, name, use_token in (
                        (source, "source", src_auth),
                        (target, "target", dst_auth),
                    ):
                        server.payload = (
                            PAYLOAD if name == "source" else b"old"
                        )
                        server.denied = server.leaked = False
                        server.expected = (
                            "Bearer " + name if use_token else None
                        )
                        url = "http://127.0.0.1:{}/{}".format(
                            server.server_port,
                            "redirect" if redirect else "file",
                        )
                        if use_token:
                            token = root / (name + ".token")
                            token.write_text(name + "\n")
                            token.chmod(0o600)
                            url += "?xrdcl.http.bearertokenfile=" + quote(
                                str(token)
                            )
                        else:
                            url += "?xrdcl.http.noauth=true"
                        urls.append(url)
                    result = subprocess.run(
                        [args.xrdcp, "--nopbar", "--force"] + urls,
                        env=environment,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        timeout=15,
                    )
                    assert result.returncode == 0, result.stderr.decode(
                        errors="replace"
                    )
                    assert source.payload == target.payload == PAYLOAD
                    assert not (source.denied or target.denied)
                    assert not (source.leaked or target.leaked)
                    print(
                        "PASS redirect={} source={} target={}".format(
                            redirect, src_auth, dst_auth
                        )
                    )


if __name__ == "__main__":
    main()
