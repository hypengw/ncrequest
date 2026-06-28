#!/usr/bin/env python3

import argparse
import base64
import hashlib
import os
import socketserver
import struct
import subprocess
import sys
import threading


GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def read_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def encode_frame(opcode, payload):
    head = bytes([0x80 | opcode])
    size = len(payload)
    if size < 126:
        return head + bytes([size]) + payload
    if size <= 0xFFFF:
        return head + bytes([126]) + struct.pack("!H", size) + payload
    return head + bytes([127]) + struct.pack("!Q", size) + payload


class WebSocketHandler(socketserver.BaseRequestHandler):
    def handle(self):
        request = b""
        while b"\r\n\r\n" not in request:
            chunk = self.request.recv(4096)
            if not chunk:
                return
            request += chunk

        headers = {}
        for line in request.decode("latin1").split("\r\n")[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()

        key = headers.get("sec-websocket-key")
        if not key:
            return

        accept = base64.b64encode(hashlib.sha1((key + GUID).encode("ascii")).digest())
        self.request.sendall(
            b"HTTP/1.1 101 Switching Protocols\r\n"
            b"Upgrade: websocket\r\n"
            b"Connection: Upgrade\r\n"
            b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n"
        )

        while True:
            header = read_exact(self.request, 2)
            if header is None:
                return

            opcode = header[0] & 0x0F
            masked = (header[1] & 0x80) != 0
            size = header[1] & 0x7F
            if size == 126:
                raw = read_exact(self.request, 2)
                if raw is None:
                    return
                size = struct.unpack("!H", raw)[0]
            elif size == 127:
                raw = read_exact(self.request, 8)
                if raw is None:
                    return
                size = struct.unpack("!Q", raw)[0]

            mask = read_exact(self.request, 4) if masked else b""
            payload = read_exact(self.request, size)
            if payload is None:
                return
            if masked:
                payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))

            if opcode == 0x8:
                self.request.sendall(encode_frame(0x8, payload))
                return
            if opcode == 0x9:
                self.request.sendall(encode_frame(0xA, payload))
                continue
            if opcode in (0x1, 0x2):
                self.request.sendall(encode_frame(opcode, payload))


class ThreadingServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-executable", required=True)
    parser.add_argument("--gtest-filter", default="qt_websockets.LocalEcho*")
    args = parser.parse_args()

    with ThreadingServer(("127.0.0.1", 0), WebSocketHandler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        env = os.environ.copy()
        env["NCREQUEST_TEST_WS_URL"] = f"ws://127.0.0.1:{server.server_address[1]}/echo"
        command = [args.test_executable, "--gtest_filter=" + args.gtest_filter]
        result = subprocess.run(command, env=env)

        server.shutdown()
        thread.join(timeout=5)
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
