#!/usr/bin/env python3
"""Fixed T736 CPU-D package update. Default checks only; --apply writes.

Never reboots. Only /dev/mmcblk0 sectors24576..25211 can be written.
All inputs/device/GPT/SPL/old package are checked before a durable backup.
Any transaction error triggers restoration of the complete636-sector backup.
"""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib

DEVICE = "/dev/mmcblk0"
SECTORS = 122142720
START = 24576
COUNT = 636
OLD_COUNT = 633
ROOT_UUID = "a0085546-4166-744a-a353-fca9272b8e48"
DISK_UUID = "ab6f3888-569a-4926-9668-80941dcb40bc"
OLD_SHA = "a7e2e06cc9843a9d2fbd37514d9923f3ed8fcd23fd7fc09f30e484f1774a721d"
NEW_SHA = "f2ea2157acc3b65b2cc7416dfdba39650e7daa6679693c558fbbdcc30ef9780a"
MANIFEST_SHA = "9bc64b4dab436b998dde31ec020c3b8237fcd195ffbd0f4343ca2c5acf38f462"
SPL_SHA = "a1d821eaee602fdf8b481c38cf4790d0f8a6f31539dd9fc6c1a714c1e92bd0ea"
SPL_LENGTH = 221184
IMAGES = [
    (24577, 0x47000000, 5040, "44aeb8e56f64f2ccd80a1cd286460eda6d0232f9cc1e82618866826b23298535"),
    (24587, 0x48000000, 53924, "81f53c1d1f571c74bd907317d813a5ed56c64382479caa75edd3cec43ada6dc6"),
    (24693, 0x52000000, 265256, "3ee3a5710c5c13c9ab11b274c6b076447ab346fc7622a1d64b5aaa692f877cdf"),
]


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def command(argv, timeout=30):
    """Always reap a failed/interrupted child before any rollback can start."""
    process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        out, err = process.communicate(timeout=timeout)
    except BaseException:
        process.kill()
        process.communicate()
        raise
    require(process.returncode == 0, f"{argv[0]} rc={process.returncode}: {err.decode(errors='replace')}")
    return out


def read_sectors(start, count):
    require(0 <= start < SECTORS and 0 < count <= 4096 and start + count <= SECTORS,
            "Read outside bounded device")
    data = command(["dd", f"if={DEVICE}", "bs=512", f"skip={start}", f"count={count}",
                    "iflag=direct,fullblock", "status=none"])
    require(len(data) == count * 512, "Short direct device read")
    return data


def write_package(path):
    require(path.stat().st_size == COUNT * 512, "Write source has wrong length")
    command(["dd", f"if={path}", f"of={DEVICE}", "bs=512", f"seek={START}",
             f"count={COUNT}", "iflag=fullblock", "oflag=direct",
             "conv=notrunc,fsync", "status=none"])


def validate_package(data, expected_new):
    expected_size, expected_sha = (COUNT * 512, NEW_SHA) if expected_new else (OLD_COUNT * 512, OLD_SHA)
    require(len(data) == expected_size and digest(data) == expected_sha, "Package size/SHA mismatch")
    words = struct.unpack("<128I", data[:512])
    require(words[:4] == (0x36333754, 0x314c5053, 1, 3), "Package signature mismatch")
    header = bytearray(data[:512])
    struct.pack_into("<I", header, 16, 0)
    require(zlib.crc32(header) == words[4], "Package header CRC mismatch")
    previous = START + 1
    for i, (wanted_lba, wanted_addr, wanted_len, wanted_sha) in enumerate(IMAGES):
        if i == 2 and not expected_new:
            wanted_len, wanted_sha = 264104, "31feddf61f98efd327ed82da5018a2b156a50aaedc394d3d160b556eccffafbe"
        lba, address, length, crc = words[8 + i * 4:12 + i * 4]
        require((lba, address, length) == (wanted_lba, wanted_addr, wanted_len), "Payload layout mismatch")
        count = (length + 511) // 512
        require(lba == previous and lba + count < 32800, "Payload bounds mismatch")
        offset = (lba - START) * 512
        image = data[offset:offset + length]
        require(len(image) == length and digest(image) == wanted_sha and zlib.crc32(image) == crc,
                "Payload SHA/CRC mismatch")
        require(not any(data[offset + length:offset + count * 512]), "Nonzero payload padding")
        previous = lba + count
    require(len(data) == (previous - START) * 512, "Trailing package bytes")


def disjoint(start, end, label):
    require(end < START or start >= START + COUNT, f"Target overlaps {label}")


def gpt_header(lba):
    raw = read_sectors(lba, 1)
    require(raw[:8] == b"EFI PART", "GPT signature missing")
    revision, size, checksum, reserved = struct.unpack_from("<4I", raw, 8)
    require(revision == 0x10000 and 92 <= size <= 512 and reserved == 0, "GPT header invalid")
    check = bytearray(raw[:size]); struct.pack_into("<I", check, 16, 0)
    require(zlib.crc32(check) == checksum, "GPT header CRC invalid")
    current, backup, first, last = struct.unpack_from("<4Q", raw, 24)
    table, count, stride, table_crc = struct.unpack_from("<QIII", raw, 72)
    require(current == lba and 0 < backup < SECTORS and backup != current, "GPT LBA invalid")
    require(2 <= first <= last < SECTORS and 1 <= count <= 1024 and stride == 128, "GPT limits invalid")
    require(str(uuid.UUID(bytes_le=raw[56:72])) == DISK_UUID, "Unexpected disk UUID")
    nsectors = (count * stride + 511) // 512
    require(2 <= table < SECTORS and table + nsectors <= SECTORS, "GPT table out of bounds")
    disjoint(current, current, "GPT header")
    disjoint(table, table + nsectors - 1, "GPT table")
    entries = read_sectors(table, nsectors)[:count * stride]
    require(zlib.crc32(entries) == table_crc, "GPT table CRC invalid")
    return {"current": current, "backup": backup, "first": first, "last": last,
            "header_sha256": digest(raw), "entries_sha256": digest(entries)}, entries


def validate_gpt():
    primary, entries = gpt_header(1)
    backup, backup_entries = gpt_header(primary["backup"])
    require(backup["backup"] == 1 and entries == backup_entries and
            (primary["first"], primary["last"]) == (backup["first"], backup["last"]),
            "Primary/backup GPT disagree")
    partitions = []
    for i in range(len(entries) // 128):
        entry = entries[i * 128:(i + 1) * 128]
        if entry[:16] == bytes(16):
            continue
        first, last = struct.unpack_from("<QQ", entry, 32)
        guid = str(uuid.UUID(bytes_le=entry[16:32]))
        require(primary["first"] <= first <= last <= primary["last"], "Partition outside GPT usable range")
        disjoint(first, last, f"partition {i + 1}")
        partitions.append({"number": i + 1, "first": first, "last": last, "partuuid": guid})
    ordered = sorted(partitions, key=lambda p: p["first"])
    require(all(a["last"] < b["first"] for a, b in zip(ordered, ordered[1:])), "Partitions overlap")
    root = [p for p in partitions if p["partuuid"] == ROOT_UUID]
    require(len(root) == 1 and root[0]["number"] == 4 and root[0]["first"] == 368640,
            "Expected root partition missing")
    return {"primary": primary, "backup": backup, "partitions": partitions}


def validate_device():
    require(os.geteuid() == 0, "Must run as root")
    device = Path(DEVICE)
    require(device.resolve() == device and stat.S_ISBLK(device.stat().st_mode), "mmcblk0 is not expected block node")
    require(Path("/sys/block/mmcblk0/device/type").read_text().strip() == "MMC", "Not MMC")
    require(int(Path("/sys/block/mmcblk0/size").read_text()) == SECTORS, "Unexpected MMC capacity")
    require(Path("/sys/block/mmcblk0/queue/logical_block_size").read_text().strip() == "512", "Not512B logical sectors")
    require(Path("/sys/block/mmcblk0/ro").read_text().strip() == "0", "MMC is read-only")
    root_device = Path("/dev/mmcblk0p4").stat()
    require(stat.S_ISBLK(root_device.st_mode), "Root device not block")
    root_number = f"{os.major(root_device.st_rdev)}:{os.minor(root_device.st_rdev)}"
    mounts = [line.split() for line in Path("/proc/self/mountinfo").read_text().splitlines()]
    require(len([m for m in mounts if m[4] == "/" and m[2] == root_number]) == 1,
            "Root mount is not mmcblk0p4")
    require(command(["blkid", "-s", "PARTUUID", "-o", "value", "/dev/mmcblk0p4"]).decode().strip().lower() == ROOT_UUID,
            "Root PARTUUID mismatch")
    require(f"root=PARTUUID={ROOT_UUID}" in Path("/proc/cmdline").read_text().split(), "Kernel root argument mismatch")
    header = read_sectors(16, 1)
    require(header[4:12] == b"eGON.BT0", "SPL magic invalid")
    length = struct.unpack_from("<I", header, 16)[0]
    require(length == SPL_LENGTH, "Actual SPL length is not pinned v4")
    spl = read_sectors(16, length // 512)
    require(digest(spl) == SPL_SHA, "Actual SPL SHA is not v4")
    check = bytearray(spl); checksum = struct.unpack_from("<I", check, 12)[0]
    struct.pack_into("<I", check, 12, 0x5f0a6c39)
    require(sum(struct.unpack(f"<{length // 4}I", check)) & 0xffffffff == checksum, "SPL eGON checksum invalid")
    return {"device": DEVICE, "sectors": SECTORS, "root_partuuid": ROOT_UUID,
            "spl_bytes": length, "spl_sha256": digest(spl), "gpt": validate_gpt()}


def fsync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try: os.fsync(fd)
    finally: os.close(fd)


def save(path, data):
    with path.open("xb") as output:
        output.write(data); output.flush(); os.fsync(output.fileno())
    os.chmod(path, 0o600)
    fsync_dir(path.parent)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--apply", action="store_true", help="Actually write the fixed636-sector range after backup")
    args = parser.parse_args()
    require(os.geteuid() == 0, "Must run as root")
    with open("/run/lock/t736-cpu-d-update.lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        package, manifest_data = args.package.read_bytes(), args.manifest.read_bytes()
        validate_package(package, True)
        require(digest(manifest_data) == MANIFEST_SHA, "Manifest SHA mismatch")
        manifest = json.loads(manifest_data)
        require((manifest["sha256"], manifest["bytes"], manifest["start_lba"], manifest["sector_count"]) ==
                (NEW_SHA, COUNT * 512, START, COUNT), "Manifest geometry mismatch")
        device = validate_device()
        before = read_sectors(START, COUNT)
        validate_package(before[:OLD_COUNT * 512], False)
        require(before[512:59904] == package[512:59904], "stage64/TF-A changed")
        left, right = read_sectors(START - 1, 1), read_sectors(START + COUNT, 1)
        result = {"status": "CHECK_PASS", "device": device, "new_sha256": NEW_SHA,
                  "backup_range_sha256": digest(before), "start_lba": START,
                  "sector_count": COUNT, "rebooted": False, "write_attempted": False}
        if not args.apply:
            print(json.dumps(result, indent=2)); return 0
        parent = Path("/var/backups/t736-cpu-d")
        parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        require(not parent.is_symlink() and parent.stat().st_uid == 0 and
                not (parent.stat().st_mode & 0o022), "Backup directory must be private root-owned")
        fsync_dir(parent.parent)
        directory = Path(tempfile.mkdtemp(prefix=datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ-"), dir=parent))
        fsync_dir(parent)
        backup = directory / "before-636-sectors.bin"
        candidate = directory / "candidate-636-sectors.bin"
        save(backup, before); save(candidate, package); save(directory / "manifest.json", manifest_data)
        save(directory / "preflight.json", (json.dumps(result, indent=2) + "\n").encode())
        require(backup.read_bytes() == before and candidate.read_bytes() == package, "Durable files verification failed")
        # Repeat live identity/layout and full old range immediately before writing.
        require(validate_device() == device and read_sectors(START, COUNT) == before,
                "Device/layout/old package changed after backup")
        result["backup_directory"] = str(directory)
        print("BACKUP_FSYNC_VERIFIED " + str(directory), flush=True)
        handlers = {s: signal.signal(s, signal.SIG_IGN) for s in (signal.SIGINT, signal.SIGTERM)}
        try:
            result["write_attempted"] = True
            try:
                write_package(candidate)
                readback = read_sectors(START, COUNT)
                require(readback == package and digest(readback) == NEW_SHA, "Direct candidate readback mismatch")
                require(read_sectors(START - 1, 1) == left and read_sectors(START + COUNT, 1) == right,
                        "Adjacent sector changed")
                require(validate_device() == device, "SPL/GPT/device changed after write")
                result["status"] = "WRITE_VERIFY_PASS"
                result["readback_sha256"] = digest(readback)
                save(directory / "readback-636-sectors.bin", readback)
                save(directory / "result.json", (json.dumps(result, indent=2) + "\n").encode())
            except BaseException as error:
                result["error"] = repr(error)
                try:
                    require(backup.read_bytes() == before, "Backup altered before restoration")
                    write_package(backup)
                    restored = read_sectors(START, COUNT)
                    require(restored == before, "Rollback direct readback mismatch")
                    result["status"] = "UPDATE_FAILED_ROLLBACK_VERIFIED"
                    result["rollback_sha256"] = digest(restored)
                except BaseException as rollback:
                    result["status"] = "CRITICAL_ROLLBACK_FAILED"
                    result["rollback_error"] = repr(rollback)
                # A failed success-journal fsync also rolls back. Keep the
                # failure record separate from a possibly partial result.json.
                try:
                    save(directory / "rollback-result.json", (json.dumps(result, indent=2) + "\n").encode())
                except BaseException as journal:
                    result["journal_error"] = repr(journal)
            print(json.dumps(result, indent=2), flush=True)
            return 0 if result["status"] == "WRITE_VERIFY_PASS" else 2
        finally:
            for sig, handler in handlers.items(): signal.signal(sig, handler)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(json.dumps({"status": "PREFLIGHT_OR_IO_ERROR", "error": repr(error), "rebooted": False}), flush=True)
        sys.exit(1)
