#!/usr/bin/env python3
# Minimal OTLP/HTTP receiver for the xrdmoncollect end-to-end test. It accepts
# POST requests on 127.0.0.1:<port> and, for each, appends two lines to
# <outfile>: "authz <path> <Authorization header>\n" and "<path> <body>\n";
# then replies 200. Used to assert that xrdmoncollect exports OTLP logs
# (/v1/logs) and traces (/v1/traces) and sends the bearer token.
#
# The path is not interpreted, and the "{}" reply carries no "errors":true, so
# the same mock also stands in for an OpenSearch cluster: point --os-url at it
# and _bulk bodies arrive as "/_bulk <body>" lines.
#
# An optional third argument names a control file: while it exists, the next
# POST is refused with a 400 and a Loki-style validation message (and the
# control file is removed, so exactly one POST is refused per touch). The
# refused request is recorded as a "rejected <path>" line. Used to assert that
# a permanently rejected body is quarantined rather than wedging the export.
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


def main():
    port = int(sys.argv[1])
    outfile = sys.argv[2]
    ctlfile = sys.argv[3] if len(sys.argv) > 3 else None

    class Handler(BaseHTTPRequestHandler):
        # HTTP/1.1 so "Expect: 100-continue" gets its interim response (the
        # default HTTP/1.0 never answers it, stalling clients that send it —
        # e.g. EL8 curl for any body > 1KB — for their 1s expect timeout).
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

    HTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
