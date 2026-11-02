#!/usr/bin/env python3
"""Minimal OTLP/HTTP receiver for the tests of xrdmoncollect.

Usage: otlp_mock.py <portfile> <outfile> [<ctlfile>]

Binds a free port on 127.0.0.1 and writes it to <portfile>. For each POST
request, appends two lines to <outfile>: "authz <path> <Authorization
header>" and "<path> <body>", then replies 200. Used to check that
xrdmoncollect exports OTLP logs (/v1/logs) and traces (/v1/traces), and
sends the bearer token.

The path is not interpreted, and the "{}" reply has no "errors":true, thus
the same mock also stands in for an OpenSearch cluster: point --os-url at it,
and the _bulk bodies arrive as "/_bulk <body>" lines.

While the optional <ctlfile> exists, the next POST gets a 400 with a
Loki-style validation message, and the mock removes the file, thus each
touch refuses exactly one POST. The refused request is recorded as a
"rejected <path>" line. Used to check that a body which an endpoint refuses
goes to quarantine, and does not block the export."""

import faulthandler
import os
import socketserver
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


class Server(HTTPServer):
    # HTTPServer.server_bind() looks up the name of the address with
    # socket.getfqdn(). On macOS, the reverse lookup of 127.0.0.1 can go to
    # DNS, and on a host whose name does not resolve it can wait for a long
    # time. The mock does not use the name.
    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


def main():
    # start_mock waits 30 s for the port. If the start takes 20 s, write the
    # stack of the mock to stderr, which start_mock shows on a failure.
    faulthandler.dump_traceback_later(20)

    portfile = sys.argv[1]
    outfile = sys.argv[2]
    ctlfile = sys.argv[3] if len(sys.argv) > 3 else None

    class Handler(BaseHTTPRequestHandler):
        # HTTP/1.1, thus "Expect: 100-continue" gets its interim response. The
        # default HTTP/1.0 never answers it, and clients that send it (EL8
        # curl for a body over 1 KB) wait 1 s for their expect timeout.
        protocol_version = "HTTP/1.1"

        def do_POST(self):
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length)
            authz = self.headers.get("Authorization", "")
            if ctlfile and os.path.exists(ctlfile):
                os.unlink(ctlfile)
                with open(outfile, "ab") as f:
                    f.write(b"rejected " + self.path.encode() + b"\n")
                msg = b"simulated validation error: entry too far behind"
                self.send_response(400)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(msg)))
                self.end_headers()
                self.wfile.write(msg)
                return
            with open(outfile, "ab") as f:
                f.write(b"authz " + self.path.encode() + b" "
                        + authz.encode() + b"\n")
                f.write(self.path.encode() + b" " + body + b"\n")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"{}")

        def log_message(self, *args):
            pass

    server = Server(("127.0.0.1", 0), Handler)

    # write the port to a new file, then rename it, thus a reader never sees
    # a partial port
    with open(portfile + ".new", "w") as f:
        f.write("%d\n" % server.server_address[1])
    os.rename(portfile + ".new", portfile)
    faulthandler.cancel_dump_traceback_later()

    server.serve_forever()


if __name__ == "__main__":
    main()
