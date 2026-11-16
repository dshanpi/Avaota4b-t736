#!/usr/bin/env python3
"""One bounded CONNECT diagnostic to one fixed host/IP; no general proxy."""
import argparse
import json
from pathlib import Path
import select
import socket
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="192.168.1.228")
    parser.add_argument("--port", type=int, default=18741)
    parser.add_argument("--client", default="192.168.1.53")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    remote_host, remote_ip, remote_port = "mirrors.aliyun.com", "183.3.205.11", 443
    record = {"schema": 1, "client": args.client, "host": remote_host,
              "remote_ip": remote_ip, "remote_port": remote_port,
              "scope": "single target CONNECT diagnostic; TLS bytes relayed without inspecting payload",
              "client_to_remote_bytes": 0, "remote_to_client_bytes": 0}
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.bind, args.port))
    listener.listen(1)
    listener.settimeout(1)
    started = time.monotonic()
    print(json.dumps({"ready": True, "proxy": f"http://{args.bind}:{args.port}",
                      "only_target": remote_host + ":443", "only_client": args.client}), flush=True)
    try:
        while time.monotonic() - started < 120:
            try:
                client, peer = listener.accept()
            except socket.timeout:
                continue
            with client:
                client.settimeout(5)
                if peer[0] != args.client:
                    continue
                header = b""
                while b"\r\n\r\n" not in header and len(header) < 8192:
                    chunk = client.recv(1024)
                    if not chunk:
                        break
                    header += chunk
                request = header.split(b"\r\n", 1)[0].decode("ascii", errors="replace")
                record["request"] = request
                if request != f"CONNECT {remote_host}:443 HTTP/1.1":
                    client.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
                    continue
                with socket.create_connection((remote_ip, remote_port), timeout=5) as remote:
                    client.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
                    tunnel_started = time.monotonic()
                    while time.monotonic() - tunnel_started < 45:
                        ready, _, _ = select.select([client, remote], [], [], 10)
                        if not ready:
                            break
                        eof = False
                        for source in ready:
                            data = source.recv(65536)
                            if not data:
                                eof = True
                                break
                            if source is client:
                                remote.sendall(data)
                                record["client_to_remote_bytes"] += len(data)
                            else:
                                client.sendall(data)
                                record["remote_to_client_bytes"] += len(data)
                        if eof:
                            break
                    record["tunnel_seconds"] = time.monotonic() - tunnel_started
                    record["completed"] = True
                break
    except Exception as exc:
        record["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        listener.close()
        record["duration_seconds"] = time.monotonic() - started
        args.output.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(record), flush=True)
    return 0 if record.get("completed") else 1


if __name__ == "__main__":
    raise SystemExit(main())
