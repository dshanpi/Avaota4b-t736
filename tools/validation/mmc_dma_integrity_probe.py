#!/usr/bin/env python3
"""Bounded O_DIRECT integrity probe using one new ordinary file (at most 32 MiB).

Every 4 KiB block is SHAKE-256(seed, phase, absolute byte offset), rather than a
repeated short template. No raw devices, cache dropping or system changes.
Failure preserves the scratch file and JSON evidence. Requires Linux/Python 3.
"""

import argparse
import ctypes
import hashlib
import json
import mmap
import os
from pathlib import Path
import random
import resource
import stat
import struct
import sys
import time
import uuid


BLOCK = 4096
LARGE = 65536
MIB = 1048576
SEED = b"t736-mmc-dma-integrity-v1\0"


class IntegrityError(Exception):
    def __init__(self, details):
        self.details = details
        super().__init__(json.dumps(details, sort_keys=True))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def payload(size, phase):
    result = bytearray(size)
    for offset in range(0, size, BLOCK):
        result[offset:offset + BLOCK] = hashlib.shake_256(
            SEED + struct.pack(">IQ", phase, offset)).digest(BLOCK)
    return result


def verify(actual, expected, offset):
    """Compare every 4 KiB block; retain exact first bad byte and block hashes."""
    for relative in range(0, len(actual), BLOCK):
        seen = actual[relative:relative + BLOCK]
        wanted = expected[offset + relative:offset + relative + BLOCK]
        if seen != wanted:
            first = next((i for i, (a, b) in enumerate(zip(seen, wanted))
                          if a != b), min(len(seen), len(wanted)))
            start = max(0, first - 8)
            raise IntegrityError({
                "kind": "payload_mismatch", "read_offset": offset,
                "read_length": len(actual), "block_offset": offset + relative,
                "first_bad_byte_offset": offset + relative + first,
                "expected_block_sha256": sha(wanted),
                "actual_block_sha256": sha(seen),
                "sample_offset": offset + relative + start,
                "expected_sample_hex": bytes(wanted[start:start + 32]).hex(),
                "actual_sample_hex": bytes(seen[start:start + 32]).hex(),
            })


def self_test():
    expected = payload(2 * LARGE, 1)
    verify(expected, expected, 0)
    verify(expected[BLOCK:BLOCK + LARGE], expected, BLOCK)
    cases = {}
    cases["1024_byte_rotation"] = expected[1024:] + expected[:1024]
    swapped = expected[:]
    swapped[1024:2048], swapped[8192:9216] = swapped[8192:9216], swapped[1024:2048]
    cases["1024_byte_descriptor_swap"] = swapped
    swapped = expected[:]
    swapped[:BLOCK], swapped[BLOCK:2 * BLOCK] = swapped[BLOCK:2 * BLOCK], swapped[:BLOCK]
    cases["4096_byte_block_swap"] = swapped
    cases["previous_phase_data"] = payload(len(expected), 0)
    flipped = expected[:]
    flipped[LARGE + 731] ^= 1
    cases["single_bit_error"] = flipped
    caught = {}
    for name, data in cases.items():
        try:
            verify(data, expected, 0)
        except IntegrityError as error:
            caught[name] = error.details["first_bad_byte_offset"]
        else:
            raise AssertionError("Did not detect " + name)
    return {"status": "PASS", "correct_payload_accepted": True,
            "nonzero_offset_accepted": True, "detected_mutations": caught,
            "unique_1024_byte_fragments": len({bytes(expected[i:i + 1024])
                                                for i in range(0, len(expected), 1024)})}


def read_optional(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def metrics_start():
    io = {}
    for line in (read_optional("/proc/self/io") or "").splitlines():
        name, value = line.split(":", 1)
        io[name] = int(value.strip())
    usage = resource.getrusage(resource.RUSAGE_SELF)
    return {"monotonic": time.monotonic(), "process_cpu_seconds": time.process_time(),
            "user_seconds": usage.ru_utime, "system_seconds": usage.ru_stime,
            "minor_faults": usage.ru_minflt, "major_faults": usage.ru_majflt,
            "voluntary_switches": usage.ru_nvcsw,
            "involuntary_switches": usage.ru_nivcsw, "proc_self_io": io}


def metrics_finish(start, count=None):
    end = metrics_start()
    result = {name: end[name] - start[name] for name in start if name != "proc_self_io"}
    result["elapsed_seconds"] = result.pop("monotonic")
    result["proc_self_io_delta"] = {name: value - start["proc_self_io"].get(name, 0)
                                    for name, value in end["proc_self_io"].items()}
    if count is not None:
        result["bytes"] = count
        result["mib_per_second"] = count / MIB / max(result["elapsed_seconds"], 1e-9)
    return result


def mount_info(path):
    """Report longest matching mounted path; do not guess a block device."""
    matches = []
    for line in (read_optional("/proc/self/mounts") or "").splitlines():
        fields = line.split()
        if len(fields) < 4:
            continue
        mountpoint = fields[1].replace("\\040", " ").replace("\\134", "\\")
        if str(path) == mountpoint or str(path).startswith(mountpoint.rstrip("/") + "/"):
            matches.append({"source": fields[0], "mountpoint": mountpoint,
                            "filesystem": fields[2], "options": fields[3]})
    return max(matches, key=lambda item: len(item["mountpoint"]), default=None)


def write_phase(fd, expected, buffer, chunk, result):
    start = metrics_start()
    completed = 0
    result.update({"chunk_bytes": chunk, "status": "RUNNING"})
    try:
        view = memoryview(buffer)[:chunk]
        try:
            for offset in range(0, len(expected), chunk):
                view[:] = expected[offset:offset + chunk]
                count = os.pwritev(fd, [view], offset)
                if count != chunk:
                    raise IntegrityError({"kind": "short_write", "offset": offset,
                                          "expected_bytes": chunk, "actual_bytes": count})
                completed += count
        finally:
            view.release()
        fsync_start = time.monotonic()
        os.fsync(fd)
        result["fsync_seconds"] = time.monotonic() - fsync_start
        result["fsync_completed"] = True
        result["status"] = "PASS"
    finally:
        result.update(metrics_finish(start, completed))


def read_pass(fd, expected, buffer, offsets, chunk, result):
    start = metrics_start()
    actual_digest = hashlib.sha256()
    expected_digest = hashlib.sha256()
    completed = 0
    result.update({"chunk_bytes": chunk, "read_count": len(offsets), "status": "RUNNING"})
    try:
        view = memoryview(buffer)[:chunk]
        try:
            for index, offset in enumerate(offsets):
                result["last_read_index"] = index
                result["last_read_offset"] = offset
                # A missed DMA transfer must not inherit a valid prior read.
                view[:] = b"\xa5" * chunk
                count = os.preadv(fd, [view], offset)
                if count != chunk:
                    raise IntegrityError({"kind": "short_read", "offset": offset,
                                          "expected_bytes": chunk, "actual_bytes": count})
                actual = bytes(view)
                verify(actual, expected, offset)
                actual_digest.update(actual)
                expected_digest.update(expected[offset:offset + chunk])
                completed += count
        finally:
            view.release()
        result["status"] = "PASS"
    finally:
        result.update(metrics_finish(start, completed))
        result["actual_sha256_of_verified_reads"] = actual_digest.hexdigest()
        result["expected_sha256_of_verified_reads"] = expected_digest.hexdigest()


def run(args, result):
    directory = args.directory.resolve(strict=True)
    if not directory.is_dir():
        raise ValueError("Scratch directory is not a directory")
    size = args.size_mib * MIB
    path = directory / ("t736-mmc-dma-" + uuid.uuid4().hex + ".bin")
    result.update({"scratch_file": str(path), "scratch_retained": False,
                   "size_bytes": size, "maximum_file_size_bytes": 32 * MIB,
                   "raw_device_access": False, "buffered_fallback": False,
                   "kernel": os.uname().release,
                   "boot_id": read_optional("/proc/sys/kernel/random/boot_id"),
                   "cpu_online": read_optional("/sys/devices/system/cpu/online"),
                   "mount": mount_info(path), "phases": []})
    if result["mount"] and result["mount"]["filesystem"] in ("tmpfs", "ramfs"):
        raise ValueError("Scratch directory is in RAM; choose a persistent filesystem")
    flags = os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_DIRECT | os.O_CLOEXEC | os.O_NOFOLLOW
    fd = os.open(path, flags, 0o600)
    result["scratch_retained"] = True
    original_stat = os.fstat(fd)
    try:
        if not stat.S_ISREG(original_stat.st_mode):
            raise ValueError("Scratch is not a regular file")
        result["file_identity"] = {"device_major": os.major(original_stat.st_dev),
                                    "device_minor": os.minor(original_stat.st_dev),
                                    "inode": original_stat.st_ino, "mode": "0600"}
        print("Scratch file: " + str(path), file=sys.stderr, flush=True)
        with mmap.mmap(-1, LARGE) as buffer:
            address = ctypes.addressof(ctypes.c_char.from_buffer(buffer))
            result["buffer_alignment_mod_4096"] = address % BLOCK
            if address % BLOCK:
                raise RuntimeError("Anonymous mmap buffer is not 4096-byte aligned")
            for phase, chunk in enumerate((BLOCK, LARGE), start=1):
                record = {"phase": phase, "pattern_seed_hex": SEED.hex(),
                          "pattern": "SHAKE256(seed || uint32be(phase) || uint64be(absolute_4KiB_block_byte_offset)), digest(4096)",
                          "status": "RUNNING", "write": {}, "reads": []}
                result["phases"].append(record)
                generation_start = metrics_start()
                expected = payload(size, phase)
                record["expected_full_file_sha256"] = sha(expected)
                record["pattern_generation"] = metrics_finish(generation_start)
                write_phase(fd, expected, buffer, chunk, record["write"])
                if os.fstat(fd).st_size != size:
                    raise RuntimeError("Unexpected scratch file size")
                for read_chunk in (BLOCK, LARGE):
                    sequential = list(range(0, size, read_chunk))
                    read = {"order": "sequential", "covers_full_file": True}
                    record["reads"].append(read)
                    read_pass(fd, expected, buffer, sequential, read_chunk, read)
                    if read["actual_sha256_of_verified_reads"] != record["expected_full_file_sha256"]:
                        raise RuntimeError("Sequential SHA256 mismatch")
                    random_seed = 0x736000 + phase * 100 + read_chunk
                    rng = random.Random(random_seed)
                    offsets = rng.sample(range(0, size - read_chunk + 1, BLOCK),
                                         min(64, (size - read_chunk) // BLOCK + 1))
                    read = {"order": "fixed_seed_random", "random_seed": random_seed,
                            "offsets": offsets, "covers_full_file": False}
                    record["reads"].append(read)
                    read_pass(fd, expected, buffer, offsets, read_chunk, read)
                record["status"] = "PASS"
                del expected
        result["status"] = "PASS"
    finally:
        os.close(fd)
    if result["status"] == "PASS" and not args.keep_success:
        current = path.lstat()
        if current.st_dev != original_stat.st_dev or current.st_ino != original_stat.st_ino:
            raise RuntimeError("Scratch identity changed; refusing cleanup")
        path.unlink()
        result["scratch_retained"] = False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="New JSON evidence file; refuses overwrite")
    parser.add_argument("--directory", type=Path, default=Path("/var/tmp"))
    parser.add_argument("--size-mib", type=int, choices=range(1, 33), default=32)
    parser.add_argument("--keep-success", action="store_true")
    parser.add_argument("--self-test", action="store_true", help="Only exercise payload verification in memory")
    args = parser.parse_args()
    if args.self_test:
        print(json.dumps(self_test(), indent=2))
        return 0
    if not args.output:
        parser.error("--output is required unless --self-test")
    if not sys.platform.startswith("linux") or not hasattr(os, "O_DIRECT"):
        parser.error("Linux O_DIRECT is required; no buffered fallback")
    result = {"schema": "t736-mmc-dma-integrity-v1", "status": "RUNNING",
              "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "script_sha256": sha(Path(__file__).read_bytes()),
              "self_test": self_test(),
              "scope": "One new regular file, two patterns; passing is bounded integrity evidence, not a filesystem or DMA proof",
              "timing_note": "Read throughput includes compare/hash/poison overhead; proc/self/io deltas include measurement reads"}
    # Reserve the evidence pathname before any scratch write, preserving old logs.
    with args.output.open("x", encoding="utf-8") as output:
        start = metrics_start()
        try:
            run(args, result)
        except (Exception, KeyboardInterrupt) as error:
            result["status"] = "FAIL"
            result["error"] = repr(error)
            if isinstance(error, IntegrityError):
                result["failure"] = error.details
        finally:
            result["total"] = metrics_finish(start)
            output.write(json.dumps(result, indent=2) + "\n")
            output.flush()
            os.fsync(output.fileno())
    print(json.dumps(result), flush=True)
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
