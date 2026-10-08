#!/usr/bin/env python3
"""Small independent RFC 6455 peer for the asynchronous client checks."""
import argparse
import base64
import hashlib
import os
from pathlib import Path
import socketserver
import struct
import subprocess
import threading
import time


def frame(opcode, data=b""):
    size = len(data)
    suffix = bytes([size]) if size < 126 else (b"\x7e" + struct.pack("!H", size) if size < 65536 else b"\x7f" + struct.pack("!Q", size))
    return bytes([0x80 | opcode]) + suffix + data


def exact(sock, size):
    output = bytearray()
    while len(output) < size:
        part = sock.recv(size - len(output))
        if not part:
            raise EOFError()
        output.extend(part)
    return bytes(output)


def receive(sock):
    head = exact(sock, 2)
    if not head[0] & 128 or not head[1] & 128:
        raise ValueError("expected masked final frame")
    size = head[1] & 127
    if size == 126:
        size = struct.unpack("!H", exact(sock, 2))[0]
    elif size == 127:
        size = struct.unpack("!Q", exact(sock, 8))[0]
    if size > 65536:
        raise ValueError("oversized input")
    mask = exact(sock, 4)
    data = exact(sock, size)
    return head[0] & 15, bytes(value ^ mask[i % 4] for i, value in enumerate(data))


class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = False


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        sock = self.request
        sock.settimeout(3)
        try:
            request = bytearray()
            while not request.endswith(b"\r\n\r\n"):
                request.extend(exact(sock, 1))
                if len(request) > 8192:
                    raise ValueError("oversized handshake")
            lines = bytes(request).split(b"\r\n")
            mode = int(lines[0].split()[1][1:])
            headers = dict(line.split(b": ", 1) for line in lines[1:] if b": " in line)
            if headers.get(b"Authorization") != b"Bearer local-test":
                raise ValueError("missing authorization")
            if headers.get(b"Sec-WebSocket-Protocol") != b"fern.test":
                raise ValueError("missing subprotocol")
            accept = base64.b64encode(hashlib.sha1(headers[b"Sec-WebSocket-Key"] + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
            if mode == 6:
                accept = b"wrong"
            response = b"HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Protocol: fern.test\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n"
            if mode == 0:
                sock.sendall(response + frame(9, b"ping"))
                if receive(sock) != (10, b"ping"):
                    raise ValueError("automatic Pong missing")
                sock.sendall(frame(1, b"ready"))
                opcode, data = receive(sock)
                if opcode != 2 or data != b"x" * 65536:
                    raise ValueError("large frame corrupted")
                sock.sendall(frame(opcode, data))
                opcode, data = receive(sock)
                if opcode != 8:
                    raise ValueError("close missing")
                sock.sendall(frame(8, data))
            elif mode == 4:
                sock.sendall(response + frame(1, b"a") + frame(1, b"b"))
                time.sleep(0.1)
            elif mode == 7:
                sock.sendall(response + frame(1, b"ab"))
                time.sleep(0.1)
            elif mode == 5:
                sock.sendall(response + frame(1, b"last") + frame(8))
                if receive(sock)[0] != 8:
                    raise ValueError("close reply missing")
            else:
                sock.sendall(response)
                while sock.recv(1024):
                    pass
        except (EOFError, ConnectionResetError, BrokenPipeError):
            pass
        except Exception as error:
            self.server.errors.append(str(error))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    with Server(("127.0.0.1", 0), Handler) as server:
        server.errors = []
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            subprocess.run([str(args.binary.resolve()), "--gtest_filter=WebSocketConnection.*"],
                           env={**os.environ, "NCREQUEST_WS_URL": f"http://127.0.0.1:{server.server_address[1]}"},
                           check=True, timeout=20)
        finally:
            server.shutdown()
            thread.join()
    if server.errors:
        raise RuntimeError(server.errors)
