#!/usr/bin/env python3
"""Verify adapter capability boundaries against the shared HTTP peer."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True, type=Path)
parser.add_argument("--work-dir", required=True, type=Path)
parser.add_argument("--filter", default="AdapterPeer.*")
args = parser.parse_args()
work = args.work_dir.resolve()
peer = Path(__file__).resolve().parents[2] / "lifetchto/tests/peer.py"
subprocess.run([sys.executable, str(peer), "--binary", str(args.binary.resolve()),
                "--work-dir", str(work), "--filter", args.filter], check=True)
events = json.loads((work / "events.json").read_text())["http"]
requests = [event for event in events if "target" in event]
assert not any(event["target"] == "/adapter-rejected" for event in requests)
for event in requests:
    if event["method"] != "PROPFIND":
        continue
    fields = event["headers"]
    mode = dict(fields)["x-repeated-fields"]
    repeated = [value for name, value in fields if name == "x-dup"]
    assert repeated == (["a", "b"] if mode == "yes" else []), event
    assert dict(fields)["x-bin"] == "ÿ", event
for event in events:
    if event.get("upload") == "/echo" and event["size"] == 3:
        assert bytes(event["body"]) == b"abc", event
    if event.get("upload") == "/adapter-307" and event["trailers"]:
        assert event["trailers"] == ["X-End: yes"], event
        assert event["size"] == 3, event
assert not any(event.get("upload") == "/expect-final" for event in events)
print("adapter wire fields, streamed bodies and rejected-request isolation passed")
