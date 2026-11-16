#!/usr/bin/env python3
"""Create a bounded CPU-R1 boot package by replacing only source U-Boot.

No disk, SPL, GPT, rootfs, kernel, TF-A, or existing package is modified.
The generated file belongs at LBA24576, using the existing SPL v4 format.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

BASE_LBA = 24576
LIMIT_LBA = 32800
BASELINE_SHA = "a7e2e06cc9843a9d2fbd37514d9923f3ed8fcd23fd7fc09f30e484f1774a721d"
SPL_V4_SHA = "a1d821eaee602fdf8b481c38cf4790d0f8a6f31539dd9fc6c1a714c1e92bd0ea"
PINNED = [
    (24577, 0x47000000, 5040, "44aeb8e56f64f2ccd80a1cd286460eda6d0232f9cc1e82618866826b23298535"),
    (24587, 0x48000000, 53924, "81f53c1d1f571c74bd907317d813a5ed56c64382479caa75edd3cec43ada6dc6"),
    (24693, 0x52000000, 264104, "31feddf61f98efd327ed82da5018a2b156a50aaedc394d3d160b556eccffafbe"),
]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def parse(data):
    if len(data) < 512 or len(data) % 512:
        raise ValueError("Package must contain aligned 512-byte sectors")
    header = bytearray(data[:512])
    words = struct.unpack("<128I", header)
    if words[:4] != (0x36333754, 0x314c5053, 1, 3):
        raise ValueError("Unexpected source SPL package signature/version/count")
    struct.pack_into("<I", header, 16, 0)
    if zlib.crc32(header) != words[4]:
        raise ValueError("Header CRC mismatch")
    entries = []
    previous = BASE_LBA + 1
    for i, maximum in enumerate((8192, 0x200000, 0x100000)):
        sector, address, length, crc = words[8 + i * 4:12 + i * 4]
        if address != PINNED[i][1] or not 0 < length <= maximum:
            raise ValueError("Image address or size violates SPL contract")
        count = (length + 511) // 512
        if sector != previous or sector + count >= LIMIT_LBA:
            raise ValueError("Image sector bounds violate fixed package layout")
        offset = (sector - BASE_LBA) * 512
        if offset + count * 512 > len(data):
            raise ValueError("Truncated image")
        image = data[offset:offset + length]
        if zlib.crc32(image) != crc or any(data[offset + length:offset + count * 512]):
            raise ValueError("Image CRC or sector padding mismatch")
        entries.append({"sector": sector, "address": hex(address), "bytes": length,
                        "crc32": f"{crc:08x}", "sha256": sha(image), "offset": offset})
        previous = sector + count
    if len(data) != (previous - BASE_LBA) * 512:
        raise ValueError("Unexpected trailing bytes")
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--uboot", type=Path, required=True)
    parser.add_argument("--build-record", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest_path = args.output.with_suffix(".manifest.json")
    if args.output.exists() or manifest_path.exists():
        raise ValueError("Candidate or manifest already exists; do not overwrite")
    baseline = args.baseline.read_bytes()
    if sha(baseline) != BASELINE_SHA:
        raise ValueError("Baseline package differs from verified C storage update (A76 passed; A55 halted)")
    old_entries = parse(baseline)
    for entry, expected in zip(old_entries, PINNED):
        if (entry["sector"], int(entry["address"], 16), entry["bytes"], entry["sha256"]) != expected:
            raise ValueError("Baseline component differs from pinned source chain")
    uboot = args.uboot.read_bytes()
    record_data = args.build_record.read_bytes()
    record = json.loads(record_data)
    if record.get("variant") != "cpu-r1" or not record.get("host_fault_tests_passed"):
        raise ValueError("Expected tested CPU-R1 build record")
    if record.get("a76_startup_hz") != 416000000 or record.get("two_stage") is not True or \
            record.get("a76_target_hz") != 1014000000 or record.get("a55_target_hz") != 494000000 or record.get("a55_clock_writes") is not False:
        raise ValueError("CPU-R1 must use the reviewed A76 sequence and leave A55 unchanged")
    if record.get("payload_sha256") != sha(uboot) or record.get("payload_size") != len(uboot):
        raise ValueError("U-Boot does not match build record")
    if not 0 < len(uboot) <= 0x100000 or sha(uboot) == PINNED[2][3]:
        raise ValueError("Invalid or unchanged U-Boot candidate")
    offset = old_entries[2]["offset"]
    package = bytearray(baseline[:offset])
    package.extend(uboot)
    package.extend(bytes((-len(uboot)) % 512))
    struct.pack_into("<II", package, (8 + 2 * 4 + 2) * 4, len(uboot), zlib.crc32(uboot))
    struct.pack_into("<I", package, 16, 0)
    struct.pack_into("<I", package, 16, zlib.crc32(package[:512]))
    new_entries = parse(package)
    if package[512:offset] != baseline[512:offset] or new_entries[:2] != old_entries[:2]:
        raise ValueError("Non-U-Boot payload unexpectedly changed")
    allowed_header_bytes = set(range(16, 20)) | set(range(72, 80))
    if any(a != b and i not in allowed_header_bytes
           for i, (a, b) in enumerate(zip(baseline[:512], package[:512]))):
        raise ValueError("Header changed outside U-Boot length/CRC and header CRC")
    manifest = {
        "variant": "cpu-r1-boot-package", "hardware_tested": False,
        "path": args.output.name, "bytes": len(package), "sha256": sha(package),
        "sector_bytes": 512, "start_lba": BASE_LBA,
        "sector_count": len(package) // 512,
        "end_lba_exclusive": BASE_LBA + len(package) // 512,
        "required_end_below_lba": LIMIT_LBA,
        "baseline": {"path": args.baseline.name, "bytes": len(baseline), "sha256": BASELINE_SHA},
        "build_record": {"path": str(args.build_record), "sha256": sha(record_data)},
        "images": new_entries,
        "changes": ["third payload U-Boot", "third payload length and CRC", "header CRC"],
        "unchanged": ["stage64 bytes and padding", "TF-A bytes and padding", "all payload LBAs and load addresses"],
        "required_existing_spl": {"variant": "v4", "sha256": SPL_V4_SHA, "modified": False},
        "scope": "Candidate only; no storage writes performed. No SPL/GPT/rootfs/kernel changes.",
        "acceptance": "Root must verify current boot package, back it up, write only the bounded range, read it back, then cold-boot verify A76 1014MHz, unchanged A55 494MHz and eight online CPUs.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as output:
        output.write(package)
    with manifest_path.open("x", encoding="utf-8") as output:
        json.dump(manifest, output, indent=2)
        output.write("\n")
    if sha(args.output.read_bytes()) != manifest["sha256"]:
        raise IOError("Written candidate verification failed")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
