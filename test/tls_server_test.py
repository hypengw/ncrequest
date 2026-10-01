#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading

from http_server_test import Handler, LocalHttpServer


class TlsHandler(Handler):
    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass


def certificate(directory: Path, name: str, hostname: str) -> tuple[Path, Path]:
    cert = directory / f"{name}.pem"
    key = directory / f"{name}-key.pem"
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", str(key), "-out", str(cert), "-days", "1",
         "-subj", f"/CN={hostname}", "-addext", f"subjectAltName=DNS:{hostname}"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )
    return cert, key


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-executable", required=True)
    parser.add_argument("--gtest-filter", default="http.Tls*:http.LocalHttps*")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ncrequest-tls-") as directory:
        root = Path(directory)
        cert, key = certificate(root, "server", "localhost")
        client_cert, client_key = certificate(root, "client", "test-client")
        servers = []
        threads = []
        try:
            env = os.environ.copy()
            env["NCREQUEST_TEST_TLS_CA"] = str(cert)
            env["NCREQUEST_TEST_TLS_CLIENT_CERT"] = str(client_cert)
            env["NCREQUEST_TEST_TLS_CLIENT_KEY"] = str(client_key)
            env["NCREQUEST_TEST_TLS_WRONG_KEY"] = str(key)
            for name, require_client in (("HTTPS", False), ("MTLS", True)):
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(cert, key)
                if require_client:
                    context.verify_mode = ssl.CERT_REQUIRED
                    context.load_verify_locations(client_cert)
                server = LocalHttpServer(("127.0.0.1", 0), TlsHandler)
                server.socket = context.wrap_socket(server.socket, server_side=True)
                servers.append(server)
                thread = threading.Thread(target=server.serve_forever)
                thread.start()
                threads.append(thread)
                env[f"NCREQUEST_TEST_{name}_URL"] = f"https://localhost:{server.server_port}"
            return subprocess.run(
                [args.test_executable, f"--gtest_filter={args.gtest_filter}"],
                env=env, check=False,
            ).returncode
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join()


if __name__ == "__main__":
    raise SystemExit(main())
