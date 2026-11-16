#!/usr/bin/env python3
"""Board-initiated LAN test: bounded download/upload, hashes and counters."""
import argparse
import hashlib
import http.client
import json
from pathlib import Path
import subprocess
import tempfile
import time
from urllib.parse import urlsplit
from urllib.request import urlopen


def snapshot(interface):
    state = {}
    for name in ("speed", "duplex", "carrier", "operstate"):
        state[name] = (Path("/sys/class/net") / interface / name).read_text().strip()
    for command in (["ip", "-s", "link", "show", interface],
                    ["ethtool", interface], ["ethtool", "-S", interface],
                    ["nstat", "-asz"]):
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=10)
            state[" ".join(command)] = {"rc": result.returncode, "stdout": result.stdout,
                                        "stderr": result.stderr}
        except (OSError, subprocess.TimeoutExpired) as exc:
            state[" ".join(command)] = {"error": str(exc)}
    return state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_url")
    parser.add_argument("--interface", default="end0")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--temp-dir", default="/run")
    args = parser.parse_args()
    url = urlsplit(args.base_url.rstrip("/"))
    if url.scheme != "http" or not url.hostname or url.username or url.query:
        parser.error("expected plain private-LAN HTTP base URL")
    if args.output.exists():
        parser.error("output exists; keep prior evidence")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    record = {"schema": 1, "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
              "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "before": snapshot(args.interface), "rounds": [], "accepted": False}
    try:
        with urlopen(args.base_url + "/meta", timeout=15) as response:
            metadata = json.load(response)
        length, rounds = metadata["bytes"], metadata["rounds"]
        if not isinstance(length, int) or not 0 < length <= 128 * 1024 * 1024:
            raise ValueError("invalid payload size")
        if not isinstance(rounds, int) or not 1 <= rounds <= 10:
            raise ValueError("invalid round count")
        record["payload"] = metadata
        with tempfile.TemporaryFile(dir=args.temp_dir) as data:
            for index in range(rounds):
                sample = {"round": index + 1}
                data.seek(0)
                data.truncate()
                digest = hashlib.sha256()
                received = 0
                started = time.monotonic()
                with urlopen(args.base_url + "/payload", timeout=30) as response:
                    while received < length:
                        chunk = response.read(min(1024 * 1024, length - received))
                        if not chunk:
                            break
                        received += len(chunk)
                        digest.update(chunk)
                        data.write(chunk)
                    if response.read(1):
                        raise ValueError("unexpected extra bytes")
                seconds = time.monotonic() - started
                sample["download"] = {"bytes": received, "seconds": seconds,
                    "mbps": received * 8 / seconds / 1e6, "sha256": digest.hexdigest()}
                if received != length or digest.hexdigest() != metadata["sha256"]:
                    raise ValueError("download length or SHA256 mismatch")
                data.seek(0)
                connection = http.client.HTTPConnection(url.hostname, url.port, timeout=30)
                started = time.monotonic()
                try:
                    connection.putrequest("POST", url.path + "/payload")
                    connection.putheader("Content-Length", str(length))
                    connection.putheader("Content-Type", "application/octet-stream")
                    connection.endheaders()
                    while chunk := data.read(1024 * 1024):
                        connection.send(chunk)
                    response = connection.getresponse()
                    reply = json.loads(response.read())
                    if response.status != 200 or not reply.get("match"):
                        raise ValueError(f"server upload verification failed: {reply}")
                finally:
                    connection.close()
                seconds = time.monotonic() - started
                sample["upload"] = {"bytes": length, "seconds": seconds,
                    "mbps": length * 8 / seconds / 1e6, "server": reply}
                sample["link_speed"] = (Path("/sys/class/net") / args.interface / "speed").read_text().strip()
                record["rounds"].append(sample)
                print(json.dumps(sample), flush=True)
        record["accepted"] = True
    except Exception as exc:
        record["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        record["after"] = snapshot(args.interface)
        record["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        args.output.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"accepted": record["accepted"], "output": str(args.output),
                          "error": record.get("error")}), flush=True)
    return 0 if record["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
