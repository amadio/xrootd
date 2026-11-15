#!/usr/bin/env python3

# A client which sends single requests of the XRootD protocol, for the tests
# of requests which the clients of XRootD do not send.
#
#   raw-client.py HOST PORT auth PROTOCOL
#       Log in, then send an authentication request whose credentials are only
#       the name of the security protocol.
#   raw-client.py HOST PORT auth-empty PROTOCOL
#       Log in, then send an authentication request without credentials.
#   raw-client.py HOST PORT statx PATH...
#       Log in with the unix protocol, then send one kXR_statx request for
#       all the paths.
#   raw-client.py HOST PORT chkpoint-xeq
#       Log in, then send a kXR_chkpoint request with the subcode kXR_ckpXeq
#       and without the request that it must carry.
#
# Print the response of the server to the last request: "ok" and the bytes of
# the body as numbers, "authmore", or "error <code> <text>".

import getpass
import socket
import struct
import sys

KXR_AUTH = 3000
KXR_CHKPOINT = 3012
KXR_LOGIN = 3007
KXR_STATX = 3022
KXR_OK = 0
KXR_AUTHMORE = 4002
KXR_ERROR = 4003

KXR_CKPXEQ = 4

STREAM = b"\0\1"


def receive(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("the server closed the connection")
        data.extend(chunk)
    return bytes(data)


def response(sock):
    _, status, dlen = struct.unpack("!2sHi", receive(sock, 8))
    return status, receive(sock, dlen)


def request(sock, code, parameters, body=b""):
    sock.sendall(struct.pack("!2sH16si", STREAM, code, parameters, len(body)) + body)
    return response(sock)


def auth(sock, protocol, credentials):
    return request(sock, KXR_AUTH, struct.pack("!12s4s", b"", protocol[:4]), credentials)


def main():
    if len(sys.argv) < 4 or (sys.argv[3] != "chkpoint-xeq" and len(sys.argv) < 5):
        print(f"usage: {sys.argv[0]} HOST PORT auth|auth-empty|statx ARGUMENT...",
              file=sys.stderr)
        print(f"       {sys.argv[0]} HOST PORT chkpoint-xeq", file=sys.stderr)
        return 2

    host, port, command = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    arguments = [argument.encode() for argument in sys.argv[4:]]

    with socket.create_connection((host, port), timeout=10) as sock:
        sock.sendall(struct.pack("!iiiii", 0, 0, 0, 4, 2012))
        response(sock)

        # The login request has no body, so the authentication request is the
        # first request with one
        status, body = request(sock, KXR_LOGIN,
                               struct.pack("!i8sBBBB", 1, b"test", 0, 0, 5, 0))
        if status != KXR_OK:
            print(f"login failed with status {status}", file=sys.stderr)
            return 1

        if command == "auth":
            status, body = auth(sock, arguments[0], arguments[0] + b"\0")
        elif command == "auth-empty":
            status, body = auth(sock, arguments[0], b"")
        elif command == "statx":
            user = getpass.getuser().encode()
            status, body = auth(sock, b"unix", b"unix\0" + user + b" " + user + b"\0")
            if status != KXR_OK:
                print(f"authentication failed with status {status}", file=sys.stderr)
                return 1
            status, body = request(sock, KXR_STATX, b"", b"\n".join(arguments))
        elif command == "chkpoint-xeq":
            # File handle 0, then the reserved bytes and the subcode
            status, body = request(sock, KXR_CHKPOINT, struct.pack("!4s11sB", b"", b"", KXR_CKPXEQ))
        else:
            print(f"unknown command {command}", file=sys.stderr)
            return 2

    if status == KXR_OK:
        print(" ".join(["ok"] + [str(byte) for byte in body]))
    elif status == KXR_AUTHMORE:
        print("authmore")
    elif status == KXR_ERROR:
        code = struct.unpack("!i", body[:4])[0]
        print(f"error {code} {body[4:].rstrip(bytes(1)).decode(errors='replace')}")
    else:
        print(f"status {status}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
