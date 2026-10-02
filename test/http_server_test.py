#!/usr/bin/env python3

import argparse
import os
import subprocess
import socketserver
import sys
import tempfile
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

    def redirect_response(self) -> bool:
        target = urlsplit(self.path)
        params = parse_qs(target.query)
        if target.path == "/redirect-to":
            self.rfile.read(int(self.headers.get("Content-Length", "0")))
            code = int(params.get("code", ["302"])[0])
            self.send_payload(code, b"redirect body", extra_headers={"Location": params.get("to", ["/redirect-echo"])[0]})
            return True
        if target.path == "/redirect-echo":
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            headers = [self.headers.get(name, "") for name in ("Authorization", "Cookie", "X-Api-Key", "Content-Type")]
            self.send_payload(HTTPStatus.OK, self.command.encode() + b"\n" + body + b"\n" + "\n".join(headers).encode())
            return True
        return False

    def method_echo(self) -> None:
        if self.async_upload_response():
            return
        if self.redirect_response():
            return
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.send_payload(HTTPStatus.OK, self.command.encode("ascii") + b"\n" + body)

    def do_HEAD(self) -> None:
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Length", "123")
        self.send_header("X-Ncrequest-Method", "HEAD")
        self.send_header("Connection", "close")
        self.end_headers()

    do_PUT = method_echo
    do_PATCH = method_echo
    do_DELETE = method_echo
    do_OPTIONS = method_echo
    do_REPORT = method_echo

    def do_GET(self) -> None:
        if self.async_upload_response():
            return
        if self.redirect_response():
            return
        if self.path.startswith("/redirect-chain"):
            params = parse_qs(urlsplit(self.path).query)
            left = int(params.get("left", ["1"])[0])
            time.sleep(float(params.get("delay", ["0"])[0]))
            if left:
                self.send_payload(302, b"intermediate", extra_headers={"Location": f"/redirect-chain?left={left - 1}&delay={params.get('delay', ['0'])[0]}"})
            else:
                self.send_payload(200, b"final")
            return
        if self.path == "/redirect-stall":
            self.send_stream(302, b"x" * 100, 1, 3.0, 0.1, extra_headers={"Location": "/text"})
            return
        if self.path in ("/redirect-duplicate", "/redirect-duplicate-same"):
            self.send_response(302)
            self.send_header("Location", "/text")
            self.send_header("Location", "/text" if self.path.endswith("-same") else "/method")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if urlsplit(self.path).scheme == "http":
            self.send_payload(HTTPStatus.OK, f"proxy\n{self.path}\n".encode())
            return
        if self.path == "/method":
            self.method_echo()
            return
        target = urlsplit(self.path)

        if target.path == "/limit-head":
            params = parse_qs(target.query)
            size = int(params.get("size", ["20"])[0])
            count = int(params.get("count", ["1"])[0])
            self.wfile.write(b"HTTP/1.1 200 OK\r\n")
            for _ in range(count):
                self.wfile.write(b"X-Pad: " + b"x" * size + b"\r\n")
            self.wfile.write(b"Content-Length: 0\r\nConnection: close\r\n\r\n")
            self.close_connection = True
            return
        if target.path == "/limit-hints":
            for _ in range(8):
                self.wfile.write(b"HTTP/1.1 103 Early Hints\r\nX-Pad: hints\r\n\r\n")
            self.send_payload(HTTPStatus.OK, b"ok")
            return
        if target.path == "/limit-trailer":
            self.wfile.write(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
            self.wfile.flush()
            time.sleep(0.1)
            self.wfile.write(b"1\r\nx\r\n0\r\nX-Pad: " + b"x" * 256 + b"\r\n\r\n")
            self.close_connection = True
            return
        if target.path == "/limit-unknown":
            self.wfile.write(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + b"x" * 256)
            self.close_connection = True
            return

        if target.path == "/endpoint":
            transport = getattr(self.server, "transport", "tcp")
            body = f"{transport}\n{self.headers.get('Host')}\n{self.path}\n".encode()
            self.send_payload(HTTPStatus.OK, body)
            return

        if target.path == "/delayed-body":
            self.send_stream(HTTPStatus.OK, b"delayed body", 4096, 1.0, 0.0)
            return

        if target.path == "/informational":
            self.wfile.write(b"HTTP/1.1 100 Continue\r\n\r\n")
            self.wfile.write(b"HTTP/1.1 103 Early Hints\r\nLink: </style.css>\r\n\r\n")
            self.wfile.flush()
            self.send_payload(HTTPStatus.OK, b"final response")
            return

        if target.path == "/empty-trailer":
            self.send_response(HTTPStatus.OK)
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"0\r\nX-Ncrequest-Trailer: completed\r\n\r\n")
            self.wfile.flush()
            return

        if target.path == "/truncated-body":
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Length", "32")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"partial")
            self.wfile.flush()
            return

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

        if target.path == "/redirect":
            self.send_payload(
                HTTPStatus.FOUND,
                b"",
                extra_headers={"Location": "/text", "X-Redirect-Only": "first"},
            )
            return

        if target.path == "/headers/request-repeat":
            values = self.headers.get_all("X-Ncrequest-Repeat") or []
            self.send_payload(HTTPStatus.OK, ("|".join(values) + "\n").encode("ascii"))
            return

        if target.path == "/headers/response-repeat":
            body = b"repeated response headers\n"
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.send_header("X-Ncrequest-Test", "local-http")
            self.send_header("X-Ncrequest-Repeat", "one")
            self.send_header("X-Ncrequest-Repeat", "two")
            self.send_header("Set-Cookie", "first=one; Path=/")
            self.send_header("Set-Cookie", "second=two; HttpOnly")
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            return

        if target.path == "/headers/trailer":
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Trailer", "X-Ncrequest-Trailer")
            self.send_header("Connection", "close")
            self.send_header("X-Ncrequest-Test", "local-http")
            self.end_headers()
            self.wfile.write(
                b"4\r\nbody\r\n0\r\nX-Ncrequest-Trailer: completed\r\n\r\n"
            )
            self.wfile.flush()
            return

        if target.path == "/missing":
            self.send_payload(HTTPStatus.NOT_FOUND, b"missing\n")
            return

        if target.path == "/server-error":
            self.send_payload(HTTPStatus.INTERNAL_SERVER_ERROR, b"server error\n")
            return

        if target.path == "/timeout-stall":
            self.send_stream(HTTPStatus.OK, b"late body", 4096, 3.0, 0.0)
            return

        if target.path == "/timeout-trickle":
            self.send_stream(HTTPStatus.OK, b"x" * 100, 1, 0.0, 0.02)
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

    def async_upload_response(self) -> bool:
        if not self.path.startswith("/async-"):
            return False
        if self.path == "/async-early":
            self.send_payload(413, b"rejected")
            self.close_connection = True
            return True
        if self.path == "/async-stall":
            time.sleep(3)
            self.close_connection = True
            return True
        try:
            duplex = self.path == "/async-duplex"
            if duplex:
                self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: 262144\r\nConnection: close\r\n\r\n")
                self.wfile.write(b"d" * 262143)
                self.wfile.flush()
            body = bytearray()
            if self.headers.get("Transfer-Encoding") == "chunked":
                while True:
                    line = self.rfile.readline()
                    if not line:
                        return True
                    size = int(line.strip(), 16)
                    if not size:
                        self.rfile.readline()
                        break
                    part = self.rfile.read(size)
                    if len(part) != size:
                        return True
                    body.extend(part)
                    self.rfile.read(2)
            else:
                size = int(self.headers.get("Content-Length", "0"))
                body.extend(self.rfile.read(size))
                if len(body) != size:
                    return True
            if duplex:
                self.wfile.write(b"d")
                self.wfile.flush()
                self.close_connection = True
            elif self.path == "/async-redirect":
                self.send_payload(307, b"", extra_headers={"Location": "/text"})
            else:
                self.send_payload(200, self.command.encode() + b"\n" + bytes(body))
        except (OSError, ValueError):
            pass
        return True

    def do_POST(self) -> None:
        if self.async_upload_response():
            return
        if self.redirect_response():
            return
        if self.path == "/body-reader":
            body = bytearray()
            if self.headers.get("Transfer-Encoding") == "chunked":
                while True:
                    size = int(self.rfile.readline().strip(), 16)
                    if size == 0:
                        self.rfile.readline()
                        break
                    body.extend(self.rfile.read(size))
                    self.rfile.read(2)
            else:
                body.extend(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
            self.send_payload(HTTPStatus.OK, bytes(body))
            return
        if self.path == "/slow-upload":
            remaining = int(self.headers.get("Content-Length", "0"))
            try:
                while remaining:
                    chunk = self.rfile.read(min(4096, remaining))
                    if not chunk:
                        return
                    remaining -= len(chunk)
                    time.sleep(0.005)
            except (ConnectionResetError, OSError):
                return
            self.send_payload(HTTPStatus.OK, b"uploaded")
            return
        if self.path == "/method":
            self.method_echo()
            return
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
    parser.add_argument("--unix-socket", action="store_true")
    parser.add_argument("--proxy", action="store_true")
    parser.add_argument("--timeout", action="store_true")
    parser.add_argument("extra_args", nargs=argparse.REMAINDER)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    server = LocalHttpServer(("127.0.0.1", 0), Handler)
    host, port = server.server_address
    thread = threading.Thread(target=server.serve_forever)
    thread.start()

    unix_server = None
    unix_thread = None
    socket_dir = None
    stall_server = None
    stall_thread = None

    try:
        env = os.environ.copy()
        env["NCREQUEST_TEST_HTTP_BASE_URL"] = f"http://{host}:{port}"
        if args.unix_socket:
            class UnixHttpServer(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
                daemon_threads = True
                transport = "unix"

            socket_dir = tempfile.TemporaryDirectory(prefix="ncrequest-http-")
            socket_path = os.path.join(socket_dir.name, "http.sock")
            unix_server = UnixHttpServer(socket_path, Handler)
            unix_thread = threading.Thread(target=unix_server.serve_forever)
            unix_thread.start()
            env["NCREQUEST_TEST_UNIX_SOCKET"] = socket_path
            env["http_proxy"] = "http://127.0.0.1:1"
            env["no_proxy"] = "127.0.0.1,localhost"

        if args.timeout:
            class StallHandler(socketserver.BaseRequestHandler):
                def handle(self):
                    self.request.recv(4096)
                    time.sleep(3.0)

            class StallServer(socketserver.ThreadingTCPServer):
                daemon_threads = True

            stall_server = StallServer(("127.0.0.1", 0), StallHandler)
            stall_thread = threading.Thread(target=stall_server.serve_forever)
            stall_thread.start()
            env["NCREQUEST_TEST_TLS_STALL_URL"] = f"https://127.0.0.1:{stall_server.server_address[1]}/"

        if args.proxy:
            env["NCREQUEST_TEST_PROXY_URL"] = env["NCREQUEST_TEST_HTTP_BASE_URL"]
            env["http_proxy"] = env["NCREQUEST_TEST_PROXY_URL"]
            env["all_proxy"] = env["NCREQUEST_TEST_PROXY_URL"]
            env["no_proxy"] = "localhost,explicit-bypass.invalid"
            for key in ("HTTP_PROXY", "ALL_PROXY", "NO_PROXY"):
                env.pop(key, None)

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
        if stall_server is not None:
            stall_server.shutdown()
            stall_server.server_close()
        if stall_thread is not None:
            stall_thread.join()
        if unix_server is not None:
            unix_server.shutdown()
            unix_server.server_close()
        if unix_thread is not None:
            unix_thread.join()
        if socket_dir is not None:
            socket_dir.cleanup()
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    sys.exit(main())
