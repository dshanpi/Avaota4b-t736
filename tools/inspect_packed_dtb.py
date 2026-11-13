#!/usr/bin/env python3
"""Extract and inspect structurally valid FDTs from an existing board image.

Only hardware facts are rendered. A packed DTB is not proof of the live FDT.
No third-party Python packages, device access, or flash operations are used.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

MAGIC = b"\xd0\x0d\xfe\xed"


def parse_fdt(blob):
    if len(blob) < 40:
        raise ValueError("truncated FDT header")
    magic, total, off_struct, off_strings, off_rsv, version, compat, cpu, nstrings, nstruct = struct.unpack_from(
        ">10I", blob
    )
    if magic != 0xD00DFEED or version != 17 or compat > version or not 40 <= total <= len(blob):
        raise ValueError("unsupported or invalid FDT header")
    if off_struct % 4 or off_rsv % 8:
        raise ValueError("unaligned FDT block")
    for start, size in ((off_struct, nstruct), (off_strings, nstrings)):
        if start < 40 or start + size > total:
            raise ValueError("FDT block outside total size")
    if max(off_struct, off_strings) < min(off_struct + nstruct, off_strings + nstrings):
        raise ValueError("overlapping FDT structure and strings")
    reservations = []
    r = off_rsv
    while True:
        if r < 40 or r + 16 > total:
            raise ValueError("unterminated reservation map")
        address, size = struct.unpack_from(">QQ", blob, r)
        r += 16
        if not address and not size:
            break
        reservations.append({"address": hex(address), "size": hex(size)})
    if any(max(off_rsv, start) < min(r, start + size) for start, size in ((off_struct, nstruct), (off_strings, nstrings))):
        raise ValueError("reservation map overlaps another block")
    strings = blob[off_strings:off_strings + nstrings]
    cursor, end = off_struct, off_struct + nstruct
    stack, nodes = [], {}
    ended = False
    while cursor + 4 <= end:
        token = struct.unpack_from(">I", blob, cursor)[0]
        cursor += 4
        if token == 1:
            nul = blob.find(b"\0", cursor, end)
            if nul < 0:
                raise ValueError("unterminated node name")
            name = blob[cursor:nul].decode("ascii")
            if "/" in name or (not stack and name):
                raise ValueError("invalid node name or root")
            stack.append(name)
            path = "/" + "/".join(stack[1:])
            if path in nodes:
                raise ValueError("duplicate node")
            nodes[path] = {}
            cursor = (nul + 4) & ~3
        elif token == 2:
            if not stack:
                raise ValueError("unbalanced node end")
            stack.pop()
        elif token == 3:
            if not stack or cursor + 8 > end:
                raise ValueError("property outside node or truncated")
            size, nameoff = struct.unpack_from(">II", blob, cursor)
            cursor += 8
            nul = strings.find(b"\0", nameoff)
            if nameoff >= len(strings) or nul < 0 or cursor + size > end:
                raise ValueError("invalid property bounds")
            name = strings[nameoff:nul].decode("ascii")
            path = "/" + "/".join(stack[1:])
            if name in nodes[path]:
                raise ValueError("duplicate property")
            nodes[path][name] = blob[cursor:cursor + size]
            cursor = (cursor + size + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            if stack or "/" not in nodes:
                raise ValueError("unfinished FDT tree")
            ended = True
            break
        else:
            raise ValueError("unknown FDT token")
    if not ended:
        raise ValueError("missing FDT end token")
    return nodes, {"total_size": total, "version": version, "boot_cpuid_phys": cpu, "reservations": reservations}


def render_value(value):
    if not value:
        return True
    if value.endswith(b"\0") and all(value[:-1].split(b"\0")) and all(x == 0 or 32 <= x < 127 for x in value):
        parts = value[:-1].decode("ascii").split("\0")
        return parts[0] if len(parts) == 1 else parts
    if len(value) % 4 == 0:
        return [f"0x{x:08x}" for x in struct.unpack(">" + "I" * (len(value) // 4), value)]
    return {"hex": value.hex()}


def inventory(nodes):
    phandles = {}
    for path, props in nodes.items():
        for name in ("phandle", "linux,phandle"):
            if name in props and len(props[name]) == 4:
                phandles[struct.unpack(">I", props[name])[0]] = path
    selected = {}
    words = ("uart", "serial", "mmc", "sdc", "memory", "reserved", "regulator", "pmu", "pmic", "axp", "pinctrl", "clock", "ccu", "timer", "psci", "interrupt-controller", "cpu@", "r-pio", "pwm")
    refs = set()
    for path, props in nodes.items():
        compatible = props.get("compatible", b"").decode("ascii", errors="replace")
        if path in ("/", "/chosen", "/aliases", "/cpus") or any(word in (path + compatible).lower() for word in words):
            # Skip large binary sequences; these are facts, not replacement DTS code.
            selected[path] = {k: render_value(v) for k, v in props.items() if len(v) <= 1024}
            for key, value in props.items():
                if (key.endswith("-supply") or key.startswith("pinctrl-")) and len(value) % 4 == 0:
                    refs.update(struct.unpack(">" + "I" * (len(value) // 4), value))
    for ref in refs:
        path = phandles.get(ref)
        if path:
            selected[path] = {k: render_value(v) for k, v in nodes[path].items() if len(v) <= 1024}
    return {"nodes": selected, "phandles": {hex(k): v for k, v in phandles.items()}}


def inspect(image, expected_sha256, output):
    data = image.read_bytes()
    actual = hashlib.sha256(data).hexdigest()
    if actual != expected_sha256:
        raise ValueError("source image hash differs from expected baseline")
    candidates, cursor = [], 0
    while True:
        offset = data.find(MAGIC, cursor)
        if offset < 0:
            break
        cursor = offset + 4
        try:
            nodes, header = parse_fdt(data[offset:])
        except (ValueError, UnicodeError, struct.error):
            continue
        size = header["total_size"]
        dtb = data[offset:offset + size]
        candidates.append((offset, dtb, nodes, header))
    if not candidates:
        raise ValueError("no structurally valid FDT found")
    output.mkdir(parents=True, exist_ok=True)
    written = []
    for offset, dtb, nodes, header in candidates:
        stem = f"{image.name}.fdt-{offset:08x}"
        facts = {
            "source_image": image.name, "source_sha256": actual,
            "offset": offset, "dtb_sha256": hashlib.sha256(dtb).hexdigest(),
            "evidence_scope": "packed_image_only_not_live_fdt",
            "header": header, "node_count": len(nodes), **inventory(nodes),
        }
        dtb_path, json_path = output / (stem + ".dtb"), output / (stem + ".json")
        encoded = (json.dumps(facts, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
        if dtb_path.exists() and dtb_path.read_bytes() != dtb:
            raise ValueError("refusing to overwrite different DTB evidence: " + str(dtb_path))
        if json_path.exists():
            previous = json.loads(json_path.read_text(encoding="utf-8"))
            if any(previous.get(key) != facts[key] for key in ("source_sha256", "dtb_sha256", "offset")):
                raise ValueError("refusing to replace facts from a different source")
        dtb_path.write_bytes(dtb)
        json_path.write_bytes(encoded)
        written.append({"offset": offset, "size": len(dtb), "nodes": len(nodes), "facts": str(json_path)})
    return written


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--expected-sha256", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(inspect(args.image, args.expected_sha256, args.output), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
