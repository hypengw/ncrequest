#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Dict, Optional
from urllib.parse import parse_qs, urlsplit


def large_body() -> bytes:
    return (b"0123456789abcdef" * 8192) + b"tail\n"


def download_body() -> bytes:
    return bytes(((i * 37 + 11) % 256 for i in range(256 * 1024)))


def slow_stream_body() -> bytes:
    return (b"slow-stream-body-" * 8192) + b"done\n"


class LocalHttpServer(ThreadingHTTPServer):
    daemon_threads = True


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args: object) -> None:
        return

    def send_payload(
        self,
        status: HTTPStatus,
        body: bytes,
        content_type: str = "text/plain; charset=utf-8",
        extra_headers: Optional[Dict[str, str]] = None,
    ) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.send_header("X-Ncrequest-Test", "local-http")
        if extra_headers:
            for name, value in extra_headers.items():
                self.send_header(name, value)
        self.end_headers()
        if body:
            try:
                self.wfile.write(body)
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                return

    def send_stream(
        self,
        status: HTTPStatus,
        body: bytes,
        chunk_size: int,
        initial_delay: float,
        chunk_delay: float,
        content_type: str = "application/octet-stream",
        extra_headers: Optional[Dict[str, str]] = None,
    ) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.send_header("X-Ncrequest-Test", "local-http")
        if extra_headers:
            for name, value in extra_headers.items():
                self.send_header(name, value)
        self.end_headers()
        try:
            self.wfile.flush()
            time.sleep(initial_delay)
            for offset in range(0, len(body), chunk_size):
                self.wfile.write(body[offset : offset + chunk_size])
                self.wfile.flush()
                time.sleep(chunk_delay)
        except (BrokenPipeError, ConnectionResetError, OSError):
            return

    def do_GET(self) -> None:
        target = urlsplit(self.path)

        if target.path == "/text":
            self.send_payload(HTTPStatus.OK, b"ncrequest python http server body\n")
            return

        if target.path == "/large":
            self.send_payload(HTTPStatus.OK, large_body())
            return

        if target.path == "/download.bin":
            self.send_payload(
                HTTPStatus.OK,
                download_body(),
                content_type="application/octet-stream",
                extra_headers={"Content-Disposition": 'attachment; filename="download.bin"'},
            )
            return

        if target.path == "/empty":
            self.send_payload(HTTPStatus.NO_CONTENT, b"")
            return

        if target.path == "/missing":
            self.send_payload(HTTPStatus.NOT_FOUND, b"missing\n")
            return

        if target.path == "/server-error":
            self.send_payload(HTTPStatus.INTERNAL_SERVER_ERROR, b"server error\n")
            return

        if target.path == "/delay":
            time.sleep(0.5)
            self.send_payload(HTTPStatus.OK, b"delayed\n")
            return

        if target.path == "/slow-first-byte":
            time.sleep(1.5)
            self.send_payload(HTTPStatus.OK, b"too late\n")
            return

        if target.path == "/slow-stream":
            self.send_stream(
                HTTPStatus.OK,
                slow_stream_body(),
                chunk_size=4096,
                initial_delay=0.2,
                chunk_delay=0.005,
            )
            return

        if target.path == "/cookie/echo":
            cookie = self.headers.get("Cookie", "")
            self.send_payload(HTTPStatus.OK, cookie.encode("utf-8") + b"\n")
            return

        if target.path in ("/cookie/set", "/cookie/slow-set", "/cookie/redirect-set"):
            query = parse_qs(target.query, keep_blank_values=True)
            name = query.get("name", [""])[0]
            value = query.get("value", [""])[0]
            if not name:
                self.send_payload(HTTPStatus.BAD_REQUEST, b"missing cookie name\n")
                return

            headers = {"Set-Cookie": f"{name}={value}; Path=/"}
            if target.path == "/cookie/redirect-set":
                headers["Location"] = "/cookie/echo"
                self.send_payload(HTTPStatus.FOUND, b"", extra_headers=headers)
            elif target.path == "/cookie/slow-set":
                self.send_stream(
                    HTTPStatus.OK,
                    b"cookie set slowly\n",
                    chunk_size=4,
                    initial_delay=0.2,
                    chunk_delay=0.02,
                    extra_headers=headers,
                )
            else:
                self.send_payload(HTTPStatus.OK, b"cookie set\n", extra_headers=headers)
            return

        self.send_payload(HTTPStatus.NOT_FOUND, b"unknown path\n")

    def do_POST(self) -> None:
        if self.path not in ("/echo", "/upload"):
            self.send_payload(HTTPStatus.NOT_FOUND, b"unknown path\n")
            return

        content_length = self.headers.get("Content-Length")
        if content_length is None:
            self.send_payload(HTTPStatus.LENGTH_REQUIRED, b"missing content length\n")
            return

        body = self.rfile.read(int(content_length))
        self.send_payload(
            HTTPStatus.OK,
            body,
            extra_headers={
                "X-Ncrequest-Method": "POST",
                "X-Ncrequest-Upload-Size": str(len(body)),
            },
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-executable", required=True)
    parser.add_argument("--gtest-filter", default="http.LocalHttp*")
    parser.add_argument("extra_args", nargs=argparse.REMAINDER)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    server = LocalHttpServer(("127.0.0.1", 0), Handler)
    host, port = server.server_address
    thread = threading.Thread(target=server.serve_forever)
    thread.start()

    try:
        env = os.environ.copy()
        env["NCREQUEST_TEST_HTTP_BASE_URL"] = f"http://{host}:{port}"

        command = [
            args.test_executable,
            f"--gtest_filter={args.gtest_filter}",
        ]
        if args.extra_args and args.extra_args[0] == "--":
            command.extend(args.extra_args[1:])
        else:
            command.extend(args.extra_args)

        completed = subprocess.run(command, env=env, check=False)
        return completed.returncode
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    sys.exit(main())
