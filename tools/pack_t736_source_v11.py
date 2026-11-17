#!/usr/bin/env python3
"""DRAFT source-v11 packer: default check only; no board/flash access.

Requires completed isolated rootfs/U-Boot builds. Keeps the frozen SPL v4,
stage64 and TF-A bytes. New boot arguments need separate cold-boot acceptance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib

from pack_armbian_raw_v6 import gpt, LAYOUT, SECTORS, file_digest
from validation.source_v11_plan import load_plan, require
from validation.spl_pack_guard import load_spl
from validation.cpu_d_pack_guard import load_cpu_d_package


def checked_build(root, plan):
    out = root / 'build/source-uboot-source-v11/output'
    record = json.loads((out / 'build-record.json').read_text())
    old = json.loads((root / 'build/source-uboot-cpu-d/output/build-record.json').read_text())
    require(record.get('variant') == 'source-v11' and record.get('host_fault_tests_passed') is True,
            'New U-Boot build has not passed source fault tests')
    for key in ('a76_startup_hz', 'a76_target_hz', 'a55_target_hz', 'two_stage',
                'dcdc3_dcdc4_required_selector_uv', 'a55_d_buffered_trace', 'a55_d_n_readback_windows'):
        require(record.get(key) == old.get(key), 'CPU-D behavior changed: ' + key)
    for name, digest in old['sources_sha256'].items():
        require(record.get('sources_sha256', {}).get(name) == digest and file_digest(root / name) == digest,
                'Frozen CPU-D input changed: ' + name)
    for name, digest in record['sources_sha256'].items():
        require(file_digest(root / name) == digest, 'New U-Boot input changed: ' + name)
    old_config = (root / 'build/source-uboot-cpu-d/output/.config').read_text()
    expected = '\n'.join('CONFIG_BOOTCOMMAND=' + json.dumps(plan['bootcommand'])
                         if line.startswith('CONFIG_BOOTCOMMAND=') else line
                         for line in old_config.splitlines()) + '\n'
    require((out / '.config').read_text() == expected, 'Expected only the persistent boot-command delta')
    require(record.get('bootcommand') == plan['bootcommand']
            and record.get('storage_mode') == plan['storage_mode']
            and record.get('input_manifest_sha256') == plan['manifest_sha256'], 'Boot plan differs from build')
    require(file_digest(out / '.config') == record['config_sha256'], 'New configuration hash mismatch')
    binary = (out / 'u-boot.bin').read_bytes()
    require(len(binary) == record['payload_size'] and hashlib.sha256(binary).hexdigest() == record['payload_sha256'],
            'New U-Boot payload mismatch')
    require(file_digest(out / 'u-boot') == record['elf_sha256'], 'New U-Boot ELF mismatch')
    return binary, record


def make_package(old_package, old_items, uboot):
    require(0 < len(uboot) <= 0x100000, 'New U-Boot exceeds the fixed loader bound')
    images = []
    for item in old_items[:2]:
        offset = (item['sector'] - 24576) * 512
        images.append((int(item['address'], 16), old_package[offset:offset + item['bytes']]))
    images.append((0x52000000, uboot))
    words = [0] * 128
    words[:4] = [0x36333754, 0x314c5053, 1, 3]
    body = bytearray()
    items = []
    for index, (address, binary) in enumerate(images):
        sector = 24577 + len(body) // 512
        words[8 + index * 4:12 + index * 4] = [sector, address, len(binary), zlib.crc32(binary)]
        body.extend(binary + bytes((-len(binary)) % 512))
        items.append({'sector': sector, 'address': hex(address), 'bytes': len(binary),
                      'sha256': hashlib.sha256(binary).hexdigest()})
    header = bytearray(struct.pack('<128I', *words))
    struct.pack_into('<I', header, 16, zlib.crc32(header))
    package = header + body
    require(24576 + len(package) // 512 < 32800, 'Boot package crosses the fixed boundary')
    for actual, previous in zip(items[:2], old_items[:2]):
        require(all(actual[key] == previous[key] for key in ('sector', 'address', 'bytes', 'sha256')),
                'Stage64 or TF-A changed')
    return package, items


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--check', action='store_true')
    mode.add_argument('--build', action='store_true')
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    require(root == Path('/home/ubuntu/t736-mainline'), 'Run only in the isolated build checkout')
    plan = load_plan(root, args.manifest)
    spl, spl_provenance = load_spl(root, 'v4')
    old_package, old_items, old_binding = load_cpu_d_package(root, spl, spl_provenance)
    uboot, build = checked_build(root, plan)
    package, items = make_package(old_package, old_items, uboot)
    rootfs = root / 'output/rootfs-armbian-source-v11.ext4'
    state = root / 'build/source-rootfs-v11-state'
    require((state / 'finalize.complete').is_file(), 'Finish the new rootfs before packing')
    require(rootfs.stat().st_size == LAYOUT[3][2] * 512, 'Rootfs geometry changed')
    rootfs_sha = file_digest(rootfs)
    require(rootfs_sha == Path(str(rootfs) + '.sha256').read_text().split()[0], 'Finalized rootfs changed')
    profile = json.loads((state / 'profile-manifest.json').read_text())
    require(profile.get('variant') == 'v11' and profile.get('kernel_release') == plan['kernel_release']
            and profile.get('input_manifest_sha256') == plan['manifest_sha256']
            and profile.get('bootcommand') == plan['bootcommand'], 'Rootfs and boot payload use different plans')
    installed = subprocess.run(['debugfs', '-R', 'cat /usr/share/doc/t736-gpu-profile/build-manifest.json', str(rootfs)],
                               check=True, text=True, capture_output=True)
    require(json.loads(installed.stdout) == profile, 'Rootfs embedded manifest differs')
    subprocess.run(['e2fsck', '-fn', str(rootfs)], check=True)
    record = {'schema': 1, 'variant': 'source-v11', 'status': 'CHECKED_NOT_COLD_BOOT_ACCEPTED',
              'kernel_release': plan['kernel_release'], 'storage_mode': plan['storage_mode'],
              'rootfs_sha256': rootfs_sha, 'input_manifest_sha256': plan['manifest_sha256'],
              'bootcommand': plan['bootcommand'], 'boot_images': items,
              'new_uboot_build_record': build, 'source_spl': spl_provenance,
              'cpu_d_baseline_binding': old_binding, 'software_fel_implementation_changed': False,
              'automatic_kernel_hang_fallback': False,
              'recovery_selection': 'setenv t736_recovery 1; run bootcmd',
              'mutter_experiment_included': False, 'credentials': 'Inherited privately from frozen v10; no changes',
              'gdm_auto_login': False, 'final_image_cold_boot_validated': False}
    if not args.build:
        print(json.dumps(record, indent=2))
        return
    output = root / 'output'
    bootpack = output / 't736-source-boot-package-source-v11.bin'
    raw = output / 't736-armbian-source-v11.raw.img'
    manifest = output / 't736-armbian-source-v11.manifest.json'
    require(not any(path.exists() or path.is_symlink() for path in (bootpack, raw, manifest)),
            'New output files must not already exist')
    primary, backup = gpt()
    regions = [('primary-gpt', 0, primary), ('source-spl', 16, spl),
               ('source-boot-package', 24576, package), ('gpt-compatibility-mirror', 40960, primary),
               ('backup-gpt', SECTORS - 3, backup)]
    with bootpack.open('xb') as stream:
        stream.write(package)
    records = []
    with raw.open('xb') as stream:
        stream.truncate(SECTORS * 512)
        for name, sector, data in regions:
            stream.seek(sector * 512); stream.write(data)
            records.append({'name': name, 'sector': sector, 'bytes': len(data),
                            'sha256': hashlib.sha256(data).hexdigest()})
        stream.seek(LAYOUT[3][1] * 512)
        with rootfs.open('rb') as source:
            while block := source.read(8 << 20):
                if any(block): stream.write(block)
                else: stream.seek(len(block), 1)
    records.append({'name': 'rootfs', 'sector': LAYOUT[3][1], 'bytes': rootfs.stat().st_size, 'sha256': rootfs_sha})
    with raw.open('rb') as stream:
        for item in records:
            stream.seek(item['sector'] * 512); digest = hashlib.sha256(); remaining = item['bytes']
            while remaining:
                block = stream.read(min(remaining, 8 << 20)); require(bool(block), 'Truncated raw image')
                digest.update(block); remaining -= len(block)
            require(digest.hexdigest() == item['sha256'], 'Packed region mismatch: ' + item['name'])
    subprocess.run(['sfdisk', '--verify', str(raw)], check=True)
    record['status'] = 'BUILT_NOT_COLD_BOOT_ACCEPTED'
    record['raw'] = {'path': str(raw.relative_to(root)), 'bytes': raw.stat().st_size,
                     'sha256': file_digest(raw), 'regions': records, 'partitions': LAYOUT}
    with manifest.open('x') as stream:
        json.dump(record, stream, indent=2); stream.write('\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
