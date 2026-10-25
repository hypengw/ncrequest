#!/usr/bin/env python3
"""Independent loopback WebSocket/TLS peers with native exit observations."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import socketserver
import ssl
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True
from websocket_connection import frame, exact, receive

parser = argparse.ArgumentParser()
parser.add_argument("--binary", type=Path, required=True)
parser.add_argument("--work-dir", type=Path, required=True)
parser.add_argument("--filter", default="QtWebSocket.*")
args = parser.parse_args()
work = args.work_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
events = []
lock = threading.Lock()


def event(**data):
    with lock:
        events.append(data)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        sock = self.request
        sock.settimeout(3)
        event(opened=True)
        try:
            request = bytearray()
            while not request.endswith(b"\r\n\r\n"):
                request += exact(sock, 1)
                if len(request) > 8192:
                    raise ValueError("oversized handshake")
            lines = bytes(request).split(b"\r\n")
            path = lines[0].split()[1].decode()
            headers = {k.lower(): v.strip() for line in lines[1:] if b":" in line for k, v in [line.split(b":", 1)]}
            event(path=path, tls=isinstance(sock, ssl.SSLSocket))
            if path == "/must-not-dispatch":
                raise ValueError("unsupported input dispatched")
            if path == "/http":
                sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nhttp")
                return
            if path == "/stall":
                while sock.recv(4096):
                    pass
                return
            accept = base64.b64encode(hashlib.sha1(headers[b"sec-websocket-key"] + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
            sock.sendall(b"HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
            if path in ("/terminal-close", "/terminal-error"):
                assert receive(sock) == (1, b"ready")
                if path == "/terminal-close":
                    sock.sendall(frame(8, b"\x03\xe8"))
                    assert receive(sock)[0] == 8
                return
            if path == "/burst":
                sock.sendall(b"".join(frame(2, n.to_bytes(2, "big")) for n in range(512)))
            if path == "/stream":
                time.sleep(0.06)
                sock.sendall(frame(1, b"") + frame(2, b"binary") + frame(8, b"\x03\xe8"))
                assert receive(sock)[0] == 8
                return
            if path == "/abrupt":
                time.sleep(0.03)
                return
            while True:
                opcode, data = receive(sock)
                event(opcode=opcode, size=len(data))
                if opcode == 8:
                    sock.sendall(frame(8, data))
                    return
                if opcode == 9:
                    sock.sendall(frame(10, data))
                elif opcode in (1, 2):
                    sock.sendall(frame(opcode, data))
                else:
                    raise ValueError("unexpected client frame")
        except (EOFError, ConnectionResetError, BrokenPipeError, ssl.SSLError):
            pass
        except Exception as error:
            event(error=repr(error))
        finally:
            event(closed=True)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, context=None):
        self.context = context
        super().__init__(("127.0.0.1", 0), Handler)

    def get_request(self):
        sock, address = super().get_request()
        if self.context:
            try:
                sock = self.context.wrap_socket(sock, server_side=True)
            except (OSError, ssl.SSLError):
                sock.close()
                raise
        return sock, address


cert, key = work / "ca.pem", work / "key.pem"
if not cert.exists():
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-keyout", str(key), "-out", str(cert), "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(cert, key)
servers = [Server(), Server(context)]
for server in servers:
    threading.Thread(target=server.serve_forever, daemon=True).start()
env = dict(os.environ, NCREQUEST_QT_WS_URL=f"http://127.0.0.1:{servers[0].server_address[1]}", NCREQUEST_QT_WS_TLS_URL=f"https://127.0.0.1:{servers[1].server_address[1]}/echo", NCREQUEST_QT_WS_CA=str(cert))
try:
    result = subprocess.run([str(args.binary.resolve()), "--gtest_filter=" + args.filter], env=env, timeout=30)
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        if sum(x.get("opened", False) for x in events) == sum(x.get("closed", False) for x in events):
            break
        time.sleep(0.01)
    assert not any("error" in x for x in events), events
    assert sum(x.get("opened", False) for x in events) == sum(x.get("closed", False) for x in events), events
    assert result.returncode == 0, result.returncode
finally:
    (work / "events.json").write_text(json.dumps(events, indent=2))
    for server in servers:
        server.shutdown()
        server.server_close()
print("Qt public WebSocket message, TLS and native peer exit assertions passed")
