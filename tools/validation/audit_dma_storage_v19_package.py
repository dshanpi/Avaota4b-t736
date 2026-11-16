#!/usr/bin/env python3
"""Bind the isolated storage-v19 Debian package to its built kernel artifacts."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

from update_t736_storage_v19 import archive_manifest, manifest_sha

RELEASE = "6.18.63-t736-storage-v19"
VERSION = "6.18.63-19"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def tar_members(package, control=False):
    option = "--ctrl-tarfile" if control else "--fsys-tarfile"
    result = {}
    with subprocess.Popen(["dpkg-deb", option, str(package)], stdout=subprocess.PIPE) as process:
        with tarfile.open(fileobj=process.stdout, mode="r|") as archive:
            for member in archive:
                name = member.name.removeprefix("./")
                if member.isfile():
                    with archive.extractfile(member) as stream:
                        if control:
                            result[name] = stream.read().decode("utf-8")
                        else:
                            if name.startswith("boot/vmlinuz-"):
                                packed = stream.read()
                                if not packed.startswith(b"\x1f\x8b"):
                                    raise RuntimeError("Expected the arm64 Debian gzip kernel")
                                unpacked = gzip.decompress(packed)
                                result[name] = {"size": member.size,
                                    "sha256": hashlib.sha256(packed).hexdigest(),
                                    "compression": "gzip", "uncompressed_size": len(unpacked),
                                    "uncompressed_sha256": hashlib.sha256(unpacked).hexdigest()}
                            else:
                                result[name] = {"size": member.size,
                                    "sha256": hashlib.file_digest(stream, "sha256").hexdigest()}
        if process.wait() != 0:
            raise RuntimeError(f"dpkg-deb {option} failed")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root = args.root.resolve()
    out = root / "build/linux-storage-v19"
    package = root / f"build/linux-image-{RELEASE}_{VERSION}_arm64.deb"
    if (out / "include/config/kernel.release").read_text().strip() != RELEASE:
        raise RuntimeError("Unexpected kernel release")
    controls = tar_members(package, control=True)
    full_control = archive_manifest(subprocess.check_output(['dpkg-deb', '--ctrl-tarfile', str(package)]))
    full_payload = archive_manifest(subprocess.check_output(['dpkg-deb', '--fsys-tarfile', str(package)]))
    fields = dict(line.split(": ", 1) for line in controls["control"].splitlines()
        if line and not line[0].isspace() and ": " in line)
    for key, value in {"Package": f"linux-image-{RELEASE}", "Version": VERSION,
                       "Architecture": "arm64"}.items():
        if fields.get(key) != value:
            raise RuntimeError(f"Package {key} does not match {value}")
    members = tar_members(package)
    matches = {}
    for suffix, original in {
        f"boot/vmlinuz-{RELEASE}": out / "arch/arm64/boot/Image.gz",
        f"boot/config-{RELEASE}": out / ".config",
        f"boot/System.map-{RELEASE}": out / "System.map",
        "allwinner/t736-gpu-candidate.dtb": out / "arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb",
    }.items():
        paths = [name for name in members if name.endswith(suffix)]
        if len(paths) != 1:
            raise RuntimeError(f"Expected one package member for {suffix}: {paths}")
        name = paths[0]
        if members[name]["sha256"] != sha(original):
            raise RuntimeError(f"Package member differs from build: {name}")
        if name.startswith("boot/vmlinuz-") and members[name]["uncompressed_sha256"] != sha(out / "arch/arm64/boot/Image"):
            raise RuntimeError("Decompressed package kernel differs from raw Image")
        matches[name] = {"build_file": str(original.relative_to(root)), **members[name]}
    record = {"schema": 1, "kind": "t736-storage-v19-package-audit",
        "status": "passed-static-package-audit", "board_accepted": False,
        "kernel_release": RELEASE,
        "package": {"path": str(package), "size": package.stat().st_size,
                    "sha256": sha(package), "control": fields},
        "matched_build_artifacts": matches,
        "regular_file_manifest": members,
        "complete_control_manifest": full_control,
        "complete_payload_manifest": full_payload,
        "control_manifest_sha256": manifest_sha(full_control),
        "payload_manifest_sha256": manifest_sha(full_payload),
        "maintainer_scripts": {name: value for name, value in controls.items()
            if name in {"preinst", "postinst", "prerm", "postrm"}},
        "maintainer_scripts_executed": False,
        "external_pvr_module": "Built separately; exact bytes and ABI must be bound by the guarded updater"}
    target = root / "evidence/storage-v19-package-20261116.json"
    target.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"evidence": str(target), "package": record["package"],
        "matched_build_artifacts": matches, "regular_file_count": len(members)}, indent=2))


if __name__ == "__main__":
    main()
