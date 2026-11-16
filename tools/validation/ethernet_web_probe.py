#!/usr/bin/env python3
"""Read-only targeted TCP metadata plus three credential-free curl requests."""
import argparse
import ipaddress
import json
from pathlib import Path
import socket
import struct
import subprocess
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", default="end0")
    parser.add_argument("--remote", default="183.3.205.11")
    parser.add_argument("--host", default="mirrors.aliyun.com")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    remote = socket.inet_aton(str(ipaddress.IPv4Address(args.remote)))
    if not args.host or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789.-" for c in args.host):
        parser.error("host must be a plain lowercase DNS hostname")
    if args.output.exists():
        parser.error("output exists; preserve previous evidence")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    record = {"schema": 1, "remote": args.remote, "host": args.host,
              "interface": args.interface, "packets": [], "requests": [],
              "scope": "target IP TCP 80/443 metadata only; no payload capture or network configuration changes"}
    done = threading.Event()
    capture = None

    def listen():
        try:
            while not done.is_set() and time.monotonic() - started < 40 and len(record["packets"]) < 256:
                try:
                    packet, address = capture.recvfrom(65535)
                except socket.timeout:
                    continue
                if len(packet) < 54 or packet[12:14] != b"\x08\x00":
                    continue
                ip = packet[14:]
                if ip[9] != 6 or remote not in (ip[12:16], ip[16:20]):
                    continue
                ihl = (ip[0] & 15) * 4
                tcp = ip[ihl:]
                if ihl < 20 or len(tcp) < 20:
                    continue
                source_port, target_port, seq, ack = struct.unpack("!HHII", tcp[:12])
                if not ({source_port, target_port} & {80, 443}):
                    continue
                thl = (tcp[12] >> 4) * 4
                flags = tcp[13]
                item = {"seconds": round(time.monotonic() - started, 6),
                    "direction": "out" if ip[16:20] == remote else "in",
                    "source_port": source_port, "target_port": target_port,
                    "ttl": ip[8], "ip_id": struct.unpack("!H", ip[4:6])[0],
                    "flags": "".join(name for mask, name in ((2,"S"),(16,"A"),(1,"F"),(4,"R"),(8,"P")) if flags & mask),
                    "sequence": seq, "acknowledgment": ack,
                    "window": struct.unpack("!H", tcp[14:16])[0],
                    "payload_bytes": max(0, struct.unpack("!H", ip[2:4])[0] - ihl - thl)}
                record["packets"].append(item)
        except OSError as exc:
            if not done.is_set():
                record["capture_error"] = str(exc)

    try:
        # ETH_P_ALL sees both directions; the loop immediately filters IPv4 TCP.
        capture = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
        capture.bind((args.interface, 0))
        capture.settimeout(0.25)
    except OSError as exc:
        record["capture_error"] = str(exc)
        if capture:
            capture.close()
        capture = None
    worker = threading.Thread(target=listen, daemon=True) if capture else None
    if worker:
        worker.start()
    try:
        for name, port, extras in (("https_default", 443, []),
                                   ("https_tls12", 443, ["--tls-max", "1.2", "--http1.1"]),
                                   ("http", 80, [])):
            scheme = "https" if port == 443 else "http"
            command = ["curl", "-q", "-4", "--noproxy", "*", "-v", "--connect-timeout", "4",
                "--max-time", "8", "--resolve", f"{args.host}:{port}:{args.remote}",
                "--output", "/dev/null", "--write-out",
                "HTTP=%{http_code} CONNECT=%{time_connect} TLS=%{time_appconnect} TOTAL=%{time_total} REMOTE=%{remote_ip}\\n",
                *extras, f"{scheme}://{args.host}/debian/"]
            request_start = time.monotonic() - started
            try:
                result = subprocess.run(command, capture_output=True, text=True, timeout=10)
                entry = {"name": name, "start_seconds": request_start, "returncode": result.returncode,
                         "stdout": result.stdout, "stderr": result.stderr}
            except (OSError, subprocess.TimeoutExpired) as exc:
                entry = {"name": name, "start_seconds": request_start, "error": str(exc)}
            record["requests"].append(entry)
            print(json.dumps(entry), flush=True)
    finally:
        time.sleep(0.5)
        done.set()
        if worker:
            worker.join(timeout=1)
        if capture:
            capture.close()
        args.output.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        for packet in record["packets"]:
            print(json.dumps(packet), flush=True)
        print(json.dumps({"output": str(args.output), "packet_count": len(record["packets"]),
                          "capture_error": record.get("capture_error")}), flush=True)


if __name__ == "__main__":
    main()
