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


def large_body() -> bytes:
    return (b"0123456789abcdef" * 8192) + b"tail\n"


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
            self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path == "/text":
            self.send_payload(HTTPStatus.OK, b"ncrequest python http server body\n")
            return

        if self.path == "/large":
            self.send_payload(HTTPStatus.OK, large_body())
            return

        if self.path == "/empty":
            self.send_payload(HTTPStatus.NO_CONTENT, b"")
            return

        if self.path == "/missing":
            self.send_payload(HTTPStatus.NOT_FOUND, b"missing\n")
            return

        if self.path == "/server-error":
            self.send_payload(HTTPStatus.INTERNAL_SERVER_ERROR, b"server error\n")
            return

        if self.path == "/delay":
            time.sleep(0.5)
            self.send_payload(HTTPStatus.OK, b"delayed\n")
            return

        self.send_payload(HTTPStatus.NOT_FOUND, b"unknown path\n")

    def do_POST(self) -> None:
        if self.path != "/echo":
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
            extra_headers={"X-Ncrequest-Method": "POST"},
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-executable", required=True)
    parser.add_argument("--gtest-filter", default="http.LocalHttp*")
    parser.add_argument("extra_args", nargs=argparse.REMAINDER)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
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
