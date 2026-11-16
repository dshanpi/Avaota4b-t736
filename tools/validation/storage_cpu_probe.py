#!/usr/bin/env python3
"""Bounded CPU integrity and direct-I/O measurements on the T736 board.

Raw storage is read only. Writes use a new temporary regular file and remove
only that file. No cache dropping, clock changes or CPU hotplug is performed.
"""
import argparse
import hashlib
import json
import mmap
import multiprocessing as mp
import os
from pathlib import Path
import tempfile
import time


def cpu_worker(cpu, duration, result):
    try:
        os.sched_setaffinity(0, {cpu})
        data = bytes(range(256)) * 4096
        expected = hashlib.sha256(data).digest()
        start = time.monotonic()
        count = 0
        while time.monotonic() - start < duration:
            if hashlib.sha256(data).digest() != expected:
                raise RuntimeError("CPU digest mismatch")
            count += 1
        result.put({"cpu": cpu, "ok": True, "mib": count,
                    "seconds": time.monotonic() - start})
    except Exception as error:
        result.put({"cpu": cpu, "ok": False, "error": str(error)})


def direct_read(path, size, offset=0, expected=None):
    total = 0
    digest = hashlib.sha256()
    start = time.monotonic()
    fd = os.open(path, os.O_RDONLY | os.O_DIRECT)
    try:
        os.lseek(fd, offset, os.SEEK_SET)
        with mmap.mmap(-1, 65536) as buffer:
            while total < size:
                count = os.readv(fd, [buffer])
                if count != 65536:
                    raise RuntimeError(f"Short direct read: {count}")
                digest.update(buffer)
                total += count
    finally:
        os.close(fd)
    elapsed = time.monotonic() - start
    value = digest.hexdigest()
    if expected is not None and value != expected:
        raise RuntimeError("Storage readback SHA256 mismatch")
    return {"bytes": total, "seconds": elapsed,
            "mib_per_second": total / 1048576 / elapsed, "sha256": value}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cpu-seconds", type=int, default=10, choices=range(1, 31))
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output exists; preserve earlier evidence")
    if os.geteuid() != 0 or not Path("/sys/block/mmcblk0/device/type").read_text().strip() == "MMC":
        parser.error("Requires root on the expected eMMC board")
    result = {"boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
              "kernel": os.uname().release,
              "online": Path("/sys/devices/system/cpu/online").read_text().strip(),
              "raw_write": False, "cpu_seconds": args.cpu_seconds}
    try:
        result["mmc_ios"] = Path("/sys/kernel/debug/mmc0/ios").read_text()
        queue = mp.Queue()
        processes = [mp.Process(target=cpu_worker, args=(cpu, args.cpu_seconds, queue))
                     for cpu in sorted(os.sched_getaffinity(0))]
        try:
            for process in processes:
                process.start()
            deadline = time.monotonic() + 45
            result["cpu"] = sorted([queue.get(timeout=max(0.1, deadline - time.monotonic()))
                                     for _ in processes], key=lambda x: x["cpu"])
            for process in processes:
                process.join(timeout=2)
                if process.is_alive() or process.exitcode != 0:
                    raise RuntimeError("CPU worker did not exit successfully")
        finally:
            for process in processes:
                if process.pid and process.is_alive():
                    process.terminate()
                    process.join(timeout=2)
                    if process.is_alive():
                        process.kill()
                        process.join()
        if not all(item["ok"] for item in result["cpu"]):
            raise RuntimeError("CPU integrity probe failed")
        result["raw_read"] = direct_read("/dev/mmcblk0p4", 16 * 1048576, 8 * 1048576)
        with tempfile.TemporaryDirectory(prefix="t736-storage-probe-", dir="/var/tmp") as directory:
            path = Path(directory) / "payload.bin"
            expected = hashlib.sha256()
            patterns = [bytes((index + i) % 256 for i in range(256)) * 256
                        for index in range(128)]
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_DIRECT, 0o600)
            start = time.monotonic()
            try:
                with mmap.mmap(-1, 65536) as buffer:
                    for pattern in patterns:
                        buffer[:] = pattern
                        expected.update(buffer)
                        if os.write(fd, buffer) != len(buffer):
                            raise RuntimeError("Short direct write")
                os.fsync(fd)
            finally:
                os.close(fd)
            elapsed = time.monotonic() - start
            result["file_write"] = {"bytes": 8 * 1048576, "seconds": elapsed,
                                    "mib_per_second": 8 / elapsed, "fsync": True}
            result["file_readback"] = direct_read(path, 8 * 1048576, expected=expected.hexdigest())
        result["status"] = "PASS"
    except Exception as error:
        result["status"] = "FAIL"
        result["error"] = repr(error)
        raise
    finally:
        args.output.write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
