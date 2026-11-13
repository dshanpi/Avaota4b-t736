#!/usr/bin/env python3
"""Read-only boot component inventory. Banners cannot prove source provenance."""
import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UBOOT = re.compile(rb"U-Boot ([0-9]{4}\.[0-9]{2}(?:[.A-Za-z0-9_+\-]*))")
KERNEL = re.compile(rb"Linux version ([0-9]+\.[0-9]+(?:[.A-Za-z0-9_+\-]*))")


def scan(path, row):
    path = Path(path)
    if not path.is_file():
        return {"file": row["file"], "state": "missing", "hash_matches": False}
    digest = hashlib.sha256()
    ub, linux = set(), set()
    prefix, carry = b"", b""
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            if not prefix:
                prefix = block[:16]
            digest.update(block)
            window = carry + block
            ub.update(m.decode("ascii") for m in UBOOT.findall(window))
            linux.update(m.decode("ascii") for m in KERNEL.findall(window))
            carry = window[-256:]
    problems = []
    if row["role"] == "persistent_runtime_reference":
        if not ub:
            problems.append("runtime U-Boot version unknown")
        if any(int(v.split(".")[0]) < 2025 for v in ub):
            problems.append("runtime U-Boot below 2025")
    if row["role"] == "persistent_kernel_reference":
        if not linux:
            problems.append("runtime kernel version unknown")
        if any(tuple(map(int, re.match(r"(\d+)\.(\d+)", v).groups())) < (6, 18) for v in linux):
            problems.append("runtime Linux below 6.18")
    return {
        "file": row["file"], "role": row["role"], "size": path.stat().st_size,
        "sha256": digest.hexdigest(),
        "hash_matches": digest.hexdigest() == row["sha256"] and path.stat().st_size == row["size"],
        "format_hint": "android_boot" if prefix.startswith(b"ANDROID!") else "sunxi_package" if prefix.startswith(b"sunxi-package") else "unclassified",
        "uboot_banners": sorted(ub), "kernel_banners": sorted(linux), "version_problems": problems,
        "mainline_provenance_proven": False,
    }


def inventory(directory, manifest):
    base = Path(directory).resolve()
    rows = manifest["components"]
    seen, result = set(), []
    for row in rows:
        name = row["file"]
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", name) or name in (".", "..") or name in seen:
            raise ValueError("invalid or duplicate component filename")
        seen.add(name)
        path = base / name
        if path.is_symlink() or path.resolve().parent != base:
            raise ValueError("component path escapes its directory")
        result.append(scan(path, row))
    return {
        "reference_integrity_ok": all(x["hash_matches"] for x in result),
        "version_blockers": [{"file": x["file"], "problems": x["version_problems"]} for x in result if x.get("version_problems")],
        "components": result, "flash_ready": False,
        "notice": "Reference inspection only; no source-provenance certification or hardware validation.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--components", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=ROOT / "config/reference-components.json")
    args = parser.parse_args()
    try:
        report = inventory(args.components, json.loads(args.manifest.read_text(encoding="utf-8-sig")))
        print(json.dumps(report, indent=2))
        return 0 if report["reference_integrity_ok"] else 1
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"inventory_components: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
