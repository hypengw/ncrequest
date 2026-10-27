#!/usr/bin/env python3
"""Independent QNAM cache peer; verifies actual requests and native revalidation."""
import argparse
from collections import Counter
from datetime import datetime, timedelta, timezone
from email.utils import format_datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import threading
import time

class Peer(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError):
            pass
    def log_message(self, *_):
        pass
    def do_HEAD(self):
        self.do_GET()
    def do_GET(self):
        with self.server.lock:
            self.server.events.append(dict(method=self.command, path=self.path,
                                           headers=dict(self.headers.items())))
            count = sum(e['path'] == self.path for e in self.server.events)
        if self.path.endswith('/slow'):
            time.sleep(0.3)
        conditional = self.headers.get('If-None-Match')
        stale = self.path.endswith('/stale')
        status = 304 if stale and conditional == '"version-1"' else 200
        if self.path.endswith('/redirect'):
            status = 302
        if self.path.endswith('/retry') and count == 1:
            status = 503
        body = b'' if status in (302, 304) else b'cache-body'
        self.send_response_only(status)
        now = datetime.now(timezone.utc)
        self.send_header('Date', format_datetime(now - timedelta(seconds=120) if stale and not conditional else now, usegmt=True))
        self.send_header('Cache-Control', 'no-store' if self.path.endswith(('/no-store', '/slow', '/retry')) else 'max-age=0, no-cache' if stale and not conditional else 'max-age=3600')
        self.send_header('ETag', '"version-1"')
        if status == 302:
            self.send_header('Location', '/external/fresh')
        if self.path.endswith('/set-cookie'):
            self.send_header('Set-Cookie', 'session=server; Path=/')
        if self.path.endswith('/vary'):
            self.send_header('Vary', 'X-Variant')
        chunked = self.path.endswith('/chunked')
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked')
        else:
            self.send_header('Content-Length', str(100 if self.path.endswith('/truncated') else len(body)))
        self.end_headers()
        try:
            if self.command != 'HEAD':
                if self.path.endswith('/cancel'):
                    self.wfile.write(body[:5])
                    self.wfile.flush()
                    time.sleep(0.1)
                    self.wfile.write(body[5:])
                else:
                    self.wfile.write(b'a\r\ncache-body\r\n0\r\n\r\n' if chunked else body)
                self.wfile.flush()
            if self.path.endswith('/truncated'):
                self.close_connection = True
        except (BrokenPipeError, ConnectionResetError):
            pass

p = argparse.ArgumentParser()
p.add_argument('--binary', required=True, type=Path)
p.add_argument('--work-dir', required=True, type=Path)
a = p.parse_args()
a.work_dir.mkdir(parents=True, exist_ok=True)
server = ThreadingHTTPServer(('127.0.0.1', 0), Peer)
server.daemon_threads = True
server.events = []
server.lock = threading.Lock()
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    env = dict(os.environ, NCREQUEST_QT_CACHE_URL=f'http://127.0.0.1:{server.server_port}',
               NCREQUEST_QT_CACHE_DIR=str(a.work_dir.resolve()))
    with (a.work_dir / 'test.log').open('w') as log:
        result = subprocess.run([str(a.binary.resolve()), '--gtest_filter=HttpCache.Native*:AdapterPeer.QtNativeCache*'], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=60)
    output = (a.work_dir / 'test.log').read_text()
    print(output, end='')
    (a.work_dir / 'events.json').write_text(json.dumps(server.events, indent=2))
    assert result.returncode == 0, result.returncode
    assert '[failure]' not in output
    counts = Counter(e['path'] for e in server.events)
    expected = {'/external/fresh': 3, '/external/seed': 1, '/external/stale': 2,
                '/external/chunked': 1, '/external/no-store': 2, '/external/truncated': 2,
                '/external/bypass': 2, '/external/disabled': 2,
                '/external/commit': 1, '/external/cancel': 2, '/external/store': 1, '/external/set-cookie': 1, '/external/vary': 1, '/external/head': 2, '/external/retry': 2,
                '/internal/persist': 1, '/failure/cache': 2, '/capacity/cache': 2}
    for path, count in expected.items():
        assert counts[path] == count, (path, counts[path], count)
    assert not any('/rejected' in e['path'] for e in server.events)
    stale = [e for e in server.events if e['path'] == '/external/stale']
    assert stale[1]['headers']['If-None-Match'] == '"version-1"', stale
    for path in ('/external/bypass', '/external/disabled'):
        requests = [e for e in server.events if e['path'] == path]
        fields = requests[0]['headers']
        assert fields.get('Cache-Control') == 'no-cache' and fields.get('Pragma') == 'no-cache', requests
        assert 'Cache-Control' not in requests[1]['headers'], requests
    print('QNAM native cache wire assertions passed:', dict(counts))
finally:
    server.shutdown()
    server.server_close()
