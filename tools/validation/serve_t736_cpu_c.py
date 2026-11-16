#!/usr/bin/env python3
"""Bounded exact-file HTTP delivery for one T736 CPU-C experiment."""
import argparse
import hashlib
import http.server
import json
from pathlib import Path
import threading
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--token", required=True)
parser.add_argument("--port", type=int, default=18744)
parser.add_argument("--lifetime", type=int, default=600)
args = parser.parse_args()
assert 1 <= args.lifetime <= 3600
assert len(args.token) == 32 and all(c in "0123456789abcdef" for c in args.token)
files = {
    "t736-source-boot-package-cpu-c.bin": args.root / "output/t736-source-boot-package-cpu-c.bin",
    "t736-source-boot-package-cpu-c.manifest.json": args.root / "output/t736-source-boot-package-cpu-c.manifest.json",
    "update_t736_cpu_c.py": args.root / "tools/validation/update_t736_cpu_c.py",
}
payloads = {f"/{args.token}/{name}": path.read_bytes() for name, path in files.items()}
allowed = {"192.168.1.53", "192.168.1.228", "127.0.0.1"}
deadline = time.monotonic() + args.lifetime


class Handler(http.server.BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def deliver(self, body):
        if time.monotonic() >= deadline or self.client_address[0] not in allowed or self.path not in payloads:
            self.send_error(404)
            return
        data = payloads[self.path]
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if body:
            self.wfile.write(data)

    def do_GET(self): self.deliver(True)
    def do_HEAD(self): self.deliver(False)


server = http.server.ThreadingHTTPServer(("192.168.1.228", args.port), Handler)
server.daemon_threads = True
timer = threading.Timer(args.lifetime, server.shutdown)
timer.daemon = True
timer.start()
print(json.dumps({"base_url": f"http://192.168.1.228:{args.port}/{args.token}",
                  "lifetime_seconds": args.lifetime, "allowed_clients": sorted(allowed),
                  "files": {name: {"bytes": len(payloads[f'/{args.token}/{name}']),
                                   "sha256": hashlib.sha256(payloads[f'/{args.token}/{name}']).hexdigest()}
                            for name in files}}), flush=True)
try:
    server.serve_forever(poll_interval=0.2)
finally:
    timer.cancel()
    server.server_close()
    print("EXPIRED_AND_CLOSED", flush=True)
