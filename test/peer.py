#!/usr/bin/env python3
"""Run the shared HTTP peer and verify ncrequest wire facts."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True)
parser.add_argument("--work-dir", required=True)
args = parser.parse_args()
work = Path(args.work_dir).resolve()
peer = Path(__file__).resolve().parents[2] / "lifetchto/tests/peer.py"
subprocess.run([sys.executable, str(peer), "--binary", str(Path(args.binary).resolve()), "--work-dir", str(work), "--filter", "http.Local*"], check=True)
events = json.loads((work / "events.json").read_text())
requests = [event for event in events["http"] if "target" in event]
targets = [event["target"] for event in requests]
assert targets.count("/cache") == 1
assert targets.count("/revalidate") == 2
assert "/cache-miss" not in targets
assert targets.count("/expect-final") == 1
assert not any(event.get("upload") == "/expect-final" for event in events["http"])
echo = [event for event in requests if event["target"] == "/echo"]
assert [event["method"] for event in echo] == ["PROPFIND", "POST", "POST", "POST"]
assert [value for name, value in echo[0]["headers"] if name == "x-dup"] == ["a", "b"]
assert dict(echo[0]["headers"])["x-bin"] == "ÿ"
uploads = [event for event in events["http"] if event.get("upload") == "/echo"]
assert bytes(uploads[0]["body"]) == b"custom"
assert json.loads(bytes(uploads[1]["body"])) == {"value": 1}
assert bytes(uploads[2]["body"]) == b"key=a+b"
assert bytes(uploads[3]["body"]) == b"bytes"
assert "origin.test" in [event.get("sni") for event in events["tls"]]
assert any(event.get("target") == "/ok" for event in events["unix"])
print("ncrequest wire headers, bodies, policy counts, cache, TLS and Unix assertions passed")
