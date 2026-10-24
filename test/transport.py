#!/usr/bin/env python3
"""Assert selected proxy wire facts as well as transport results."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True, type=Path)
parser.add_argument("--work-dir", required=True, type=Path)
args = parser.parse_args()
peer = Path(__file__).resolve().parents[2] / "lifetchto/tests/peer.py"
subprocess.run([sys.executable, str(peer), "--binary", str(args.binary.resolve()), "--work-dir", str(args.work_dir.resolve()), "--filter", "TransportOptions.*:TransportPeer.*:ProxyOptions.*:ProxyPeer.*", "--proxy"], check=True)
events = json.loads((args.work_dir / "events.json").read_text())
for name in ("proxy_http", "proxy_https", "proxy_socks"):
    values = events[name]
    assert sum(x.get("proxy_opened", False) for x in values) == sum(x.get("proxy_closed", False) for x in values), values
    assert not any("proxy_error" in x for x in values), values
requests = [x for x in events["proxy_http"] if "proxy_method" in x]
assert sum(x["proxy_target"].endswith("/proxy-http?q=x%2Fy") for x in requests) == 6, requests
assert sum(x["proxy_target"].endswith("/proxy-snapshot") for x in requests) == 2, requests
assert not any("#" in x["proxy_target"] or "proxy-bypass" in x["proxy_target"] for x in requests), requests
assert sum(x["proxy_method"] == "CONNECT" and x["proxy_target"].startswith("deny.test:") for x in requests) == 3, requests
socks = [(x["socks_version"], x["address_kind"], x["socks_host"]) for x in events["proxy_socks"] if "socks_version" in x]
assert socks == [(4, 1, "127.0.0.1"), (4, 3, "origin.test"), (5, 1, "127.0.0.1"), (5, 3, "origin.test"), (5, 3, "origin.test")], socks
assert any(x.get("client_certificate") for x in events["mutual"]), events.keys()
print("TLS identity, proxy selection, CONNECT, SOCKS DNS mode and peer exit passed")
