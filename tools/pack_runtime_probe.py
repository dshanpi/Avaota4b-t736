#!/usr/bin/env python3
"""Append RAM boot and update payloads inside the verified 96 MiB boot partition.

The Android image starts U-Boot; appended payloads are read explicitly by its
MMC commands. This does not change GPT or write the existing root filesystem.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

PARTITION_START = 172032
PARTITION_BYTES = 196608 * 512
SLOTS = (("kernel", 8 << 20, 0x60000000),
         ("dtb", 40 << 20, 0x63000000),
         ("initrd", 42 << 20, 0x64000000),
         ("package", 64 << 20, None))


def pack(boot, payloads):
    if boot[:8] != b"ANDROID!" or len(boot) >= SLOTS[0][1]:
        raise ValueError("Expected a small Android U-Boot boot image")
    if struct.unpack_from("<I", boot, 40)[0] != 2:
        raise ValueError("Expected the validated Android v2 header")
    image = bytearray(boot)
    records = []
    for index, (name, offset, address) in enumerate(SLOTS):
        data = payloads[name]
        limit = SLOTS[index + 1][1] if index + 1 < len(SLOTS) else PARTITION_BYTES
        sectors = (len(data) + 511) // 512
        if not data or offset + sectors * 512 > limit:
            raise ValueError(f"{name} exceeds its reserved slot")
        image.extend(bytes(offset - len(image)))
        image.extend(data)
        image.extend(bytes((-len(image)) % 512))
        record = {"name": name, "offset": offset, "size": len(data),
                  "sha256": hashlib.sha256(data).hexdigest(),
                  "crc32": f"{zlib.crc32(data):08x}", "sectors": sectors}
        if address is not None:
            sector = PARTITION_START + offset // 512
            record["load_address"] = f"0x{address:x}"
            record["command"] = f"mmc read 0x{address:x} 0x{sector:x} 0x{sectors:x}"
        records.append(record)
    return bytes(image), records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot", type=Path, required=True)
    for name, _, _ in SLOTS:
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output already exists; select a new candidate name")
    data, records = pack(args.boot.read_bytes(), {
        name: getattr(args, name).read_bytes() for name, _, _ in SLOTS})
    args.output.write_bytes(data)
    manifest = {"partition": "boot", "partition_start_sector": PARTITION_START,
                "partition_bytes": PARTITION_BYTES, "contains_rootfs_payload": False,
                "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                "payloads": records}
    args.output.with_suffix(".payloads.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
