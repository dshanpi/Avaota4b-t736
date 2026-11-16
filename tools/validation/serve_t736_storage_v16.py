#!/usr/bin/env python3
"""Serve exactly the reviewed v16 updater, kernel and PVR for one hour."""
import argparse
import hashlib
import http.server
import json
from pathlib import Path
import threading
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--token', required=True)
parser.add_argument('--port', type=int, default=18746)
parser.add_argument('--lifetime', type=int, default=3600)
args = parser.parse_args()
assert 1 <= args.lifetime <= 3600
assert len(args.token) == 32 and all(c in '0123456789abcdef' for c in args.token)
files = {
    'kernel.deb': ('build/linux-image-6.18.63-t736-storage-v16_6.18.63-16_arm64.deb',
                   '520b42929ee4700a29ab8da5f020b828948a607c87a6eb8dd98c8ecacd6170c6'),
    'pvrsrvkm.ko': ('build/pvr-storage-v16/target_aarch64/pvrsrvkm.ko',
                    'abd38db687c93b91757719564e1f0fe35b10b137f23d78ea425ff2845d8c8474'),
    'update_t736_storage_v16.py': ('tools/validation/update_t736_storage_v16.py',
                                   'cf6ad0b32e1ffbd5d6687e54d1a4204f2e7ecd24a0bc44bdeb71f76d79aded7f'),
}
payloads = {}
for name, (relative, expected) in files.items():
    data = (args.root / relative).read_bytes()
    assert hashlib.sha256(data).hexdigest() == expected, 'Reviewed input changed: ' + name
    payloads[f'/{args.token}/{name}'] = data
allowed = {'192.168.1.53', '192.168.1.228', '127.0.0.1'}
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
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        if body:
            self.wfile.write(data)

    def do_GET(self): self.deliver(True)
    def do_HEAD(self): self.deliver(False)


server = http.server.ThreadingHTTPServer(('192.168.1.228', args.port), Handler)
server.daemon_threads = True
timer = threading.Timer(args.lifetime, server.shutdown)
timer.daemon = True
timer.start()
print(json.dumps({'base_url': f'http://192.168.1.228:{args.port}/{args.token}',
                  'lifetime_seconds': args.lifetime, 'allowed_clients': sorted(allowed),
                  'files': {name: {'bytes': len(payloads[f'/{args.token}/{name}']), 'sha256': expected}
                            for name, (_, expected) in files.items()}}), flush=True)
try:
    server.serve_forever(poll_interval=0.2)
finally:
    timer.cancel()
    server.server_close()
    print('EXPIRED_AND_CLOSED', flush=True)
