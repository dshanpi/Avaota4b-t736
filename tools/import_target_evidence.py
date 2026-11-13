#!/usr/bin/env python3
"""Validate one board capture and compare its live FDT to packed-image facts.

This imports evidence only. It does not generate DTS, choose load addresses,
declare a board bootable, or operate a device.
"""
import argparse
import base64
import binascii
import hashlib
import json
import re
import struct
from pathlib import Path

from inspect_packed_dtb import inventory, parse_fdt, render_value
from workspace_policy import outside_tina


def decode_capture(text):
    text = text.replace("\r", "")
    matches = re.findall(r"^BEGIN_FDT_BASE64\n(.*?)^END_FDT_BASE64$", text, re.M | re.S)
    if len(matches) != 1:
        raise ValueError("expected exactly one complete FDT capture")
    hashes = re.findall(r"^([0-9a-fA-F]{64})[ \t]+/sys/firmware/fdt[ \t]*$", text, re.M)
    if len(hashes) != 1:
        raise ValueError("expected one board-side FDT SHA256; recollect with sha256sum")
    try:
        blob = base64.b64decode("".join(matches[0].split()), validate=True)
    except (binascii.Error, ValueError) as exc:
        raise ValueError("corrupted FDT base64") from exc
    digest = hashlib.sha256(blob).hexdigest()
    if digest != hashes[0].lower():
        raise ValueError("FDT hash mismatch: serial capture may be incomplete or corrupted")
    nodes, header = parse_fdt(blob)
    if header["total_size"] != len(blob):
        raise ValueError("unexpected trailing bytes after FDT")
    compat = nodes["/"].get("compatible", b"").split(b"\0")
    if not any(x in (b"allwinner,t736", b"arm,sun60iw2p1") for x in compat):
        raise ValueError("FDT does not identify the expected T736/sun60iw2 board")
    return blob, nodes, header, digest


def memory_banks(nodes):
    root = nodes["/"]
    def cell_count(key, default):
        raw = root.get(key, struct.pack(">I", default))
        if len(raw) != 4:
            raise ValueError("invalid root cell count")
        count = struct.unpack(">I", raw)[0]
        if count not in (1, 2):
            raise ValueError("unsupported root cell count")
        return count
    ac, sc = cell_count("#address-cells", 2), cell_count("#size-cells", 1)
    stride = 4 * (ac + sc)
    banks = []
    for path, props in nodes.items():
        if path.count("/") != 1 or props.get("device_type") != b"memory\0":
            continue
        reg = props.get("reg", b"")
        if not reg or len(reg) % stride:
            raise ValueError("invalid memory reg cells")
        for offset in range(0, len(reg), stride):
            address = int.from_bytes(reg[offset:offset + ac * 4], "big")
            size = int.from_bytes(reg[offset + ac * 4:offset + stride], "big")
            banks.append({"path": path, "address": hex(address), "size_bytes": size, "end_exclusive": hex(address + size)})
    return banks


def compare_nodes(packed_nodes, live_nodes):
    differences = []
    for path in sorted(set(packed_nodes) | set(live_nodes)):
        before, after = packed_nodes.get(path), live_nodes.get(path)
        if before is None or after is None:
            differences.append({"path": path, "change": "added" if before is None else "removed"})
            continue
        for prop in sorted(set(before) | set(after)):
            if before.get(prop) != after.get(prop):
                differences.append({"path": path, "property": prop, "packed": before.get(prop), "live": after.get(prop)})
    return differences


def import_capture(capture, output, packed=None):
    raw_capture = capture.read_bytes()
    blob, nodes, header, digest = decode_capture(raw_capture.decode("utf-8", errors="replace"))
    facts = inventory(nodes)
    banks = memory_banks(nodes)
    report = {
        "evidence_scope": "captured_running_kernel_fdt_not_independent_hardware_verification",
        "capture_sha256": hashlib.sha256(raw_capture).hexdigest(),
        "dtb_sha256": digest, "header": header, "node_count": len(nodes),
        "model": render_value(nodes["/"].get("model", b"")),
        "memory_banks": banks, "memory_total_bytes": sum(b["size_bytes"] for b in banks),
        **facts,
        "port_bootable": False,
        "remaining_verification": ["schematic_and_PMIC_rails", "DDR_training_source", "BL31_SCP_firmware_provenance_and_load_contract", "new_bootloader_and_kernel_hardware_tests"],
    }
    if packed:
        prior = json.loads(packed.read_text(encoding="utf-8-sig"))
        if prior.get("evidence_scope") != "packed_image_only_not_live_fdt":
            raise ValueError("comparison input must be a packed-DTB facts file")
        report["packed_dtb_sha256"] = prior["dtb_sha256"]
        report["differences"] = compare_nodes(prior["nodes"], facts["nodes"])
    encoded = (json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    output = outside_tina(output)
    outputs = {"effective.dtb": blob, "live-fdt-facts.json": encoded, "capture.log": raw_capture}
    for name, content in outputs.items():
        dest = output / name
        if dest.exists() and dest.read_bytes() != content:
            raise ValueError("refusing to overwrite different evidence; choose a fresh output directory")
    output.mkdir(parents=True, exist_ok=True)
    for name, content in outputs.items():
        (output / name).write_bytes(content)
    return {"output": str(output), "dtb_sha256": digest, "memory_total_bytes": report["memory_total_bytes"], "node_count": len(nodes)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--packed-facts", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(import_capture(args.capture, args.output, args.packed_facts), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f"import_target_evidence: {error}\n")


if __name__ == "__main__":
    main()
