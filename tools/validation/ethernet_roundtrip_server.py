#!/usr/bin/env python3
"""Bounded, token-path LAN payload server; exposes no host filesystem."""
import argparse
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="192.168.1.228")
    parser.add_argument("--port", type=int, default=18738)
    parser.add_argument("--size-mib", type=int, default=64)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--deadline", type=int, default=600)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    address = ipaddress.ip_address(args.bind)
    if not address.is_private or address.is_unspecified:
        parser.error("bind must be a specific private or loopback address")
    if not 1 <= args.size_mib <= 128 or not 1 <= args.rounds <= 10:
        parser.error("size must be 1..128 MiB and rounds 1..10")
    if not 30 <= args.deadline <= 900:
        parser.error("deadline must be 30..900 seconds")
    if args.output.exists():
        parser.error("output already exists; keep prior evidence")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    token = secrets.token_hex(12)
    payload = os.urandom(args.size_mib * 1024 * 1024)
    metadata = {"bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest(),
                "rounds": args.rounds}
    record = {"schema": 1, "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "listen": f"{args.bind}:{args.port}", "payload": metadata,
              "requests": [], "completed_roundtrips": 0, "accepted": False}
    client_script = Path(__file__).with_name("ethernet_roundtrip_client.py").read_bytes()
    lock = threading.Lock()

    def save():
        # Atomic updates preserve evidence even when the deadline is reached.
        temporary = args.output.with_suffix(args.output.suffix + ".tmp")
        temporary.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        temporary.replace(args.output)

    class Handler(BaseHTTPRequestHandler):
        def setup(self):
            super().setup()
            self.connection.settimeout(30)

        def reply(self, status, data, content_type="application/octet-stream"):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == f"/{token}/meta":
                self.reply(200, json.dumps(metadata).encode(), "application/json")
            elif self.path == f"/{token}/client.py":
                self.reply(200, client_script, "text/plain")
            elif self.path == f"/{token}/payload":
                started = time.monotonic()
                try:
                    self.reply(200, payload)
                except (OSError, TimeoutError) as error:
                    record["requests"].append({"method": "GET", "error": str(error)})
                else:
                    seconds = time.monotonic() - started
                    record["requests"].append({"method": "GET", "client": self.client_address[0],
                        "bytes": len(payload), "seconds": seconds,
                        "mbps": len(payload) * 8 / seconds / 1e6})
                with lock:
                    save()
            else:
                self.reply(404, b"")

        def do_POST(self):
            if self.path != f"/{token}/payload":
                return self.reply(404, b"")
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                return self.reply(400, b"bad content length\n")
            if length != len(payload) or record["completed_roundtrips"] >= args.rounds:
                return self.reply(400, b"unexpected payload\n")
            digest = hashlib.sha256()
            received = 0
            started = time.monotonic()
            error = None
            try:
                while received < length:
                    chunk = self.rfile.read(min(1024 * 1024, length - received))
                    if not chunk:
                        break
                    received += len(chunk)
                    digest.update(chunk)
            except (OSError, TimeoutError) as exc:
                error = str(exc)
            seconds = time.monotonic() - started
            match = received == length and digest.hexdigest() == metadata["sha256"]
            entry = {"method": "POST", "client": self.client_address[0], "bytes": received,
                     "sha256": digest.hexdigest(), "match": match, "seconds": seconds,
                     "mbps": received * 8 / seconds / 1e6, "error": error}
            record["requests"].append(entry)
            if match:
                record["completed_roundtrips"] += 1
            record["accepted"] = record["completed_roundtrips"] == args.rounds
            with lock:
                save()
            try:
                self.reply(200 if match else 422, json.dumps(entry).encode(), "application/json")
            finally:
                if record["accepted"]:
                    threading.Thread(target=self.server.shutdown, daemon=True).start()

        def log_message(self, fmt, *values):
            print(fmt % values, flush=True)

    server = HTTPServer((args.bind, args.port), Handler)
    timer = threading.Timer(args.deadline, server.shutdown)
    timer.daemon = True
    save()
    print(json.dumps({"ready": True, "base_url": f"http://{args.bind}:{args.port}/{token}",
                      "payload": metadata, "output": str(args.output)}), flush=True)
    timer.start()
    try:
        server.serve_forever()
    finally:
        timer.cancel()
        server.server_close()
        record["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        with lock:
            save()
    print(json.dumps({"accepted": record["accepted"],
                      "completed_roundtrips": record["completed_roundtrips"]}), flush=True)
    return 0 if record["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
