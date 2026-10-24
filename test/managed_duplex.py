#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import subprocess
import sys
parser = argparse.ArgumentParser()
parser.add_argument("--binary", type=Path, required=True)
parser.add_argument("--work-dir", type=Path, required=True)
args = parser.parse_args()
peer = Path(__file__).resolve().parents[2] / "lifetchto/tests/peer.py"
subprocess.run([sys.executable, str(peer), "--binary", str(args.binary.resolve()), "--work-dir", str(args.work_dir.resolve()), "--filter", "ManagedDuplexPeer.*", "--duplex"], check=True)
events = json.loads((args.work_dir / "events.json").read_text())
for name in ("raw_tcp", "raw_unix", "raw_tls"):
    values = events[name]
    assert sum(e.get("opened", False) for e in values) == sum(e.get("closed", False) for e in values) >= 2, values
    data = b"".join(bytes(e["data"]) for e in values if "data" in e)
    assert data == (b"helloagainhelloagain" if name == "raw_tls" else b"hellohello"), values
    assert any(e.get("input_eof") for e in values), values
print("managed TCP/UDS/TLS data, send half-close and peer exit passed")
