#!/usr/bin/env python3
"""Run the Unix byte transport checks without an external service."""
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
        self.request.settimeout(3)
        incoming = bytearray()
        try:
            while True:
                chunk = self.request.recv(1024)
                if not chunk:
                    break
                if not incoming and chunk.startswith(b"B"):
                    time.sleep(0.5)
                    return
                incoming.extend(chunk)
                if len(incoming) > 65536:
                    raise ValueError("unexpected input size")
            if incoming:
                if incoming != b"echo":
                    raise ValueError("unexpected input")
                self.request.sendall(incoming + b":after-eof")
        except (ConnectionResetError, BrokenPipeError):
            pass
        except Exception as error:
            self.server.errors.append(str(error))


def run(binary):
    with tempfile.TemporaryDirectory(prefix="ncrequest-duplex-") as directory:
        path = str(Path(directory) / "api.sock")
        with Server(path, Handler) as server:
            server.errors = []
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                subprocess.run([str(binary), "--gtest_filter=UnixDuplex.*"], check=True,
                               env={**os.environ, "NCREQUEST_DUPLEX_SOCKET": path}, timeout=15)
            finally:
                server.shutdown()
                thread.join()
        if server.errors:
            raise RuntimeError(server.errors)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    run(parser.parse_args().binary.resolve())
