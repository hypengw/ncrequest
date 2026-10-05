#!/usr/bin/env python3
"""Verify curl's raw Unix connection against a bounded HTTP upgrade peer."""
import argparse
import os
from pathlib import Path
import socketserver
import subprocess
import tempfile
import threading
import time


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = False


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            self.request.settimeout(3)
            data = bytearray()
            while not data.endswith(b"\r\n\r\n"):
                part = self.request.recv(1)
                if not part or len(data) >= 1024:
                    raise ValueError("invalid request head")
                data.extend(part)
            expected = (
                b"POST /%s HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\n"
                b"Upgrade: tcp\r\nContent-Length: 0\r\n\r\n"
            )
            route = bytes(data).split(b" ", 2)[1].removeprefix(b"/")
            if data != expected % route:
                raise ValueError("unexpected request")
            if route == b"denied":
                self.request.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 2\r\n\r\n{}")
                return
            failures = {
                b"denied-chunked": b"Transfer-Encoding: chunked\r\n\r\n1\r\n{\r\n1\r\n}\r\n0\r\nX-End: yes\r\n\r\n",
                b"denied-eof": b"Connection: close\r\n\r\n{}",
                b"denied-large": b"Content-Length: 65537\r\n\r\n",
                b"denied-truncated": b"Content-Length: 8\r\n\r\n{}",
                b"denied-ambiguous": b"Content-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n{}",
                b"denied-bad-chunk": b"Transfer-Encoding: chunked\r\n\r\nnope\r\n",
                b"denied-slow": b"Content-Length: 8\r\n\r\n{",
                b"denied-eof-large": b"Connection: close\r\n\r\n" + b"x" * 65537,
                b"denied-chunk-large": b"Transfer-Encoding: chunked\r\n\r\n10001\r\n",
            }
            if route in failures:
                self.request.sendall(b"HTTP/1.1 403 Forbidden\r\n" + failures[route])
                if route == b"denied-slow":
                    time.sleep(0.15)
                return
            if route == b"informational-loop":
                self.request.sendall(b"HTTP/1.1 103 Early Hints\r\n\r\n" * 9)
                return
            if route == b"wrong-protocol":
                self.request.sendall(b"HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: wrong\r\n\r\n")
                return
            if route == b"truncated":
                self.request.sendall(b"HTTP/1.1 101 Switching Protocols\r\nConnection:")
                return
            if route == b"oversized":
                self.request.sendall(b"HTTP/1.1 101 Switching Protocols\r\nX-Large: " + b"x" * 65536)
                return
            if route == b"slow":
                time.sleep(0.15)
                return
            fragmented = data == expected % b"fragmented"
            if not fragmented and route not in (b"coalesced", b"informational", b"informational-limit"):
                raise ValueError("unexpected request")
            response = (
                b"HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n"
                b"Upgrade: tcp\r\n\r\nfirst:"
            )
            if route in (b"informational", b"informational-limit"):
                response = b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\n" + response
            if fragmented:
                for byte in response:
                    self.request.sendall(bytes([byte]))
                    time.sleep(0.001)
            else:
                self.request.sendall(response)
            if route == b"informational-limit":
                return
            incoming = bytearray()
            while True:
                part = self.request.recv(1024)
                if not part:
                    break
                incoming.extend(part)
                if len(incoming) > 1024:
                    raise ValueError("unbounded input")
            if incoming != b"input":
                raise ValueError("input lost before half-close")
            self.request.sendall(b"tail-after-eof")
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as error:
            self.server.errors.append(str(error))


def run(binary):
    with tempfile.TemporaryDirectory(prefix="ncrequest-upgrade-") as directory:
        path = str(Path(directory) / "api.sock")
        with Server(path, Handler) as server:
            server.errors = []
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                env = {**os.environ, "NCREQUEST_UPGRADE_SOCKET": path}
                subprocess.run([str(binary), "--gtest_filter=CurlUpgradeProbe.*:HttpUpgrade.*"],
                               env=env, check=True, timeout=15)
            finally:
                server.shutdown()
                thread.join()
        if server.errors:
            raise RuntimeError(server.errors)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    run(parser.parse_args().binary.resolve())
