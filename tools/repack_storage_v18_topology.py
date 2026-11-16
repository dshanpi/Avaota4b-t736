#!/usr/bin/env python3
"""Combine the frozen SMHC3 DMA kernel with the separately validated CPU DT.

Offline only. Repack just the DTB; assert Image, PVR and every other payload
member are unchanged. Run after the parent's CPU-topology board acceptance.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import tarfile
import tempfile

RELEASE = "6.18.63-t736-storage-v18"
PACKAGE = f"linux-image-{RELEASE}_6.18.63-18_arm64.deb"
BASE_PACKAGE = "c20ae52d58f83ccf5be812d994fb44e51913edeab50dc938911fe43235bba4c7"
BASE_IMAGE = "6c671b870e204d3c3b9e8f202932d2dabd90887d8df6009f0d7d06a0d02f3b43"
TOPOLOGY_DTB = "6124591aab1cfc02b287a88872835b20a6d2dac70b3e7cd2b93bb9af36d3e82b"
FRAGMENT = "0af7e42afb7f75ba6c71c51b2bc605b329b71f8966cb90f114e5d1b48687cbc1"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(value, message):
    if not value:
        raise RuntimeError(message)


def fdt(path):
    data = path.read_bytes()
    magic, total, offset, strings, reserve, *_ = struct.unpack_from(">10I", data)
    require(magic == 0xD00DFEED and total <= len(data), "Invalid DTB")
    memory = []
    while True:
        entry = struct.unpack_from(">QQ", data, reserve)
        reserve += 16
        if entry == (0, 0):
            break
        memory.append(entry)
    stack, nodes = [], {}
    while offset < total:
        token, = struct.unpack_from(">I", data, offset)
        offset += 4
        if token == 1:
            end = data.index(0, offset)
            stack.append(data[offset:end].decode())
            offset = (end + 4) & ~3
            nodes["/" + "/".join(stack[1:])] = {}
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, name = struct.unpack_from(">II", data, offset)
            offset += 8
            end = data.index(0, strings + name)
            prop = data[strings + name:end].decode()
            nodes["/" + "/".join(stack[1:])][prop] = data[offset:offset + length]
            offset = (offset + length + 3) & ~3
        elif token == 9:
            break
        elif token != 4:
            raise RuntimeError(f"Unknown DT token {token}")
    return nodes, memory


def payload_manifest(package):
    result = {}
    with subprocess.Popen(["dpkg-deb", "--fsys-tarfile", str(package)], stdout=subprocess.PIPE) as proc:
        with tarfile.open(fileobj=proc.stdout, mode="r|") as archive:
            for item in archive:
                entry = {"type": item.type.decode(), "mode": item.mode,
                         "uid": item.uid, "gid": item.gid, "link": item.linkname}
                if item.isfile():
                    with archive.extractfile(item) as stream:
                        entry.update(size=item.size, sha256=hashlib.file_digest(stream, "sha256").hexdigest())
                result[item.name.removeprefix("./")] = entry
        require(proc.wait() == 0, "Package payload extraction failed")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--topology-dtb", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    base = root / "build/storage-v18-dmaonly-final"
    output = root / "build/storage-v18-topology-final"
    kernel = root / "build/linux-storage-v18"
    fragment = root / "boards/t736-cpu-topology-candidate.dtsi"
    require(sha(base / PACKAGE) == BASE_PACKAGE, "Wrong frozen DMA-only package")
    require(sha(base / "Image") == sha(kernel / "arch/arm64/boot/Image") == BASE_IMAGE,
            "Kernel changed after DMA acceptance")
    require(sha(args.topology_dtb) == TOPOLOGY_DTB, "Wrong CPU-topology input DTB")
    require(sha(fragment) == FRAGMENT, "Topology fragment changed")
    output.mkdir(parents=True, exist_ok=True)
    candidate = output / "t736-gpu-candidate.dtb"
    source = output / "t736-gpu-candidate.dts"
    dtc = kernel / "scripts/dtc/dtc"
    commands = [
        [str(dtc), "-I", "dtb", "-O", "dts", "-o", str(source),
         str(base / "t736-gpu-candidate.dtb")],
        [str(dtc), "-I", "dts", "-O", "dtb", "-o", str(candidate), str(source)],
    ]
    with (output / "dtc.log").open("wb") as log:
        subprocess.run(commands[0], check=True, stdout=log, stderr=log)
        source.write_text(source.read_text() + "\n" + fragment.read_text())
        subprocess.run(commands[1], check=True, stdout=log, stderr=log)
    accepted_nodes, accepted_memory = fdt(args.topology_dtb)
    nodes, memory = fdt(candidate)
    expected_extra = struct.pack(">8I", 0, 0x020025E0, 0, 4, 0, 0x0200259C, 0, 4)
    require(memory == accepted_memory, "Reservation map changed")
    require(nodes.keys() == accepted_nodes.keys(), "Unexpected DT nodes")
    for node, props in accepted_nodes.items():
        expected = dict(props)
        if node == "/mmc@4023000":
            expected["reg"] += expected_extra
        require(nodes[node] == expected, f"Unexpected DT property change: {node}")
    # dpkg-deb extraction does not execute any maintainer script.
    with tempfile.TemporaryDirectory(prefix="storage-v18-topology-", dir=root / "build") as tmp:
        stage = Path(tmp) / "root"
        subprocess.run(["dpkg-deb", "-R", str(base / PACKAGE), str(stage)], check=True)
        dt_member = stage / f"usr/lib/linux-image-{RELEASE}/allwinner/t736-gpu-candidate.dtb"
        require(dt_member.is_file(), "Expected package DT member missing")
        old_size = dt_member.stat().st_size
        shutil.copyfile(candidate, dt_member)
        dt_name = str(dt_member.relative_to(stage))
        sums = stage / "DEBIAN/md5sums"
        lines = sums.read_text().splitlines()
        selected = [i for i, line in enumerate(lines) if line.split(maxsplit=1)[1] == dt_name]
        require(len(selected) == 1, "Expected exactly one DTB checksum entry")
        lines[selected[0]] = hashlib.md5(dt_member.read_bytes()).hexdigest() + "  " + dt_name
        sums.write_text("\n".join(lines) + "\n")
        control = stage / "DEBIAN/control"
        fields = control.read_text().splitlines()
        delta_kib = (dt_member.stat().st_size - old_size + 1023) // 1024
        fields = [f"Installed-Size: {int(line.split(':', 1)[1]) + delta_kib}"
                  if line.startswith("Installed-Size:") else line for line in fields]
        control.write_text("\n".join(fields) + "\n")
        subprocess.run(["dpkg-deb", "--root-owner-group", "-Zxz", "--build",
                        str(stage), str(output / PACKAGE)], check=True)
    before, after = payload_manifest(base / PACKAGE), payload_manifest(output / PACKAGE)
    require(before.keys() == after.keys(), "Package payload member set changed")
    for name, entry in before.items():
        expected = dict(entry)
        if name == dt_name:
            expected.update(size=candidate.stat().st_size, sha256=sha(candidate))
        require(after[name] == expected, f"Unexpected package payload change: {name}")
    # Publish build candidates only after structural validation and successful packing.
    shutil.copyfile(candidate, kernel / "arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb")
    shutil.copyfile(source, root / "sources/work/linux-storage-v18/arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dts")
    shutil.copyfile(output / PACKAGE, root / "build" / PACKAGE)
    require(sha(kernel / "arch/arm64/boot/Image") == BASE_IMAGE, "Kernel changed during repack")
    build = json.loads((base / "storage-v18-build-20261116.json").read_text())
    build["variant"] = "dma-with-accepted-cpu-topology"
    build["dt_changes"].append("Preserve independently validated A55/A76 capacity and CPU-map")
    build["topology_input"] = {"sha256": TOPOLOGY_DTB, "fragment_sha256": FRAGMENT}
    build["package"].update(size=(output / PACKAGE).stat().st_size, sha256=sha(output / PACKAGE))
    for name in list(build["files"]):
        build["files"][name] = sha(root / name)
    build["repack_script"] = "tools/repack_storage_v18_topology.py"
    build["repack_script_sha256"] = sha(Path(__file__))
    (root / "evidence/storage-v18-build-20261116.json").write_text(json.dumps(build, indent=2) + "\n")
    record = {
        "schema": 1, "status": "offline-topology-merge-complete-board-v18-pending",
        "baseline_package_sha256": BASE_PACKAGE, "kernel_sha256_unchanged": BASE_IMAGE,
        "topology_input_sha256": TOPOLOGY_DTB, "fragment_sha256": FRAGMENT,
        "outside_mmc_reg_identical_to_accepted_topology": True,
        "mmc_reg_additions": ["0x020025e0/4 read-only", "0x0200259c/4 read-only"],
        "reservation_map_identical": True, "package_sha256": sha(output / PACKAGE),
        "other_payload_members_and_metadata_identical": True,
        "dtb_md5sums_updated": True,
        "candidate_dtb_sha256": sha(candidate), "candidate_dts_sha256": sha(source),
        "commands": commands, "payload_audit": "run audit_dma_storage_package.py separately",
        "hardware_access": False,
    }
    (root / "evidence/storage-v18-topology-merge-20261116.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
