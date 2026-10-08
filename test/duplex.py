#!/usr/bin/env python3
"""Run direct TCP transport checks against an isolated loopback server."""
import argparse
import os
from pathlib import Path
import socketserver
import subprocess
import threading


class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = False


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(3)
        incoming = bytearray()
        try:
            while True:
                data = self.request.recv(1024)
                if not data:
                    break
                incoming.extend(data)
                if len(incoming) > 65536:
                    raise ValueError("unexpected input size")
                if incoming.startswith(b"GET ") and b"\r\n\r\n" in incoming:
                    if b"Authorization: Bearer local-test\r\n" not in incoming:
                        raise ValueError("missing authorization")
                    self.request.sendall(b"HTTP/1.1 101 Switching Protocols\r\n"
                                         b"Connection: Upgrade\r\nUpgrade: test\r\n\r\nready")
                    return
            if incoming:
                if incoming != b"echo":
                    raise ValueError("unexpected input")
                self.request.sendall(b"echo:after-eof")
        except (ConnectionResetError, BrokenPipeError):
            pass
        except Exception as error:
            self.server.errors.append(str(error))


def run(binary):
    with Server(("127.0.0.1", 0), Handler) as server:
        server.errors = []
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            subprocess.run([str(binary), "--gtest_filter=NetworkDuplex.*"], check=True,
                           env={**os.environ, "NCREQUEST_DUPLEX_URL":
                                f"http://127.0.0.1:{server.server_address[1]}/"}, timeout=15)
        finally:
            server.shutdown()
            thread.join()
    if server.errors:
        raise RuntimeError(server.errors)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    run(parser.parse_args().binary.resolve())
