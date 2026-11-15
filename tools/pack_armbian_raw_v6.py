#!/usr/bin/env python3
"""Reproduce a legacy T736 debug image with the rejected Tina boot chain.

This is NOT the independent-boot Armbian packer. The raw contains vendor
Boot0 and U-Boot 2018 in addition to the separately supplied RAM-only loader.
Use only to reproduce an explicitly selected historical recovery/debug build.

Run on the build host. The separate Tina FES container is RAM-only and its
components are unmodified. OpenixCLI compensates the vendor's 40960-sector
offset with wrapping FES addresses. Never compensate it a second time.
"""
import argparse
import gzip
import hashlib
import json
import struct
import subprocess
import uuid
import zlib
from pathlib import Path

from pack_boot_validation import align, read_package

SECTOR = 512
SECTORS = 16777216  # 8 GiB deployment image; relocate backup GPT after boot.
LAYOUT = [('boot-resource', 73728, 65536), ('env', 139264, 32768),
          ('boot', 172032, 196608), ('rootfs', 368640, 15239680),
          ('private', 15608320, 32768), ('UDISK', 15641088, SECTORS - 15641088 - 34)]
EXPECTED = {
    'boot0_sdcard.fex': '0e2397669df804256505c786d7a9db6b40720203dd3e6b1ea33955486efa98c8',
    'boot_package.fex': 'b6dbcfb96ebac121218d30afe1d028aaad8a1a471063f05c5c1c8aaade419df3',
    'u-boot.fex': 'cdeb0be64b063a089797e9c385f0c6a20f70548e5540de3453308c53a4b546fd',
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while b := f.read(8 << 20):
            h.update(b)
    return h.hexdigest()


def gpt():
    entries = bytearray()
    for index, (name, start, size) in enumerate(LAYOUT):
        entries.extend(struct.pack('<16s16sQQQ72s',
            uuid.UUID('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7').bytes_le,
            uuid.UUID(f'a0085546-4166-744a-a353-fca9272b8e{0x45+index:02x}').bytes_le,
            start, start + size - 1, 1 << 63, name.encode('utf-16-le')))
    crc = zlib.crc32(entries)
    def header(current, backup, table):
        h = bytearray(struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000,
            92, 0, 0, current, backup, 73728, SECTORS - 34,
            uuid.UUID('ab6f3888-569a-4926-9668-80941dcb40bc').bytes_le,
            table, len(LAYOUT), 128, crc))
        struct.pack_into('<I', h, 16, zlib.crc32(h))
        return h + bytes(SECTOR - len(h))
    mbr = bytearray(SECTOR)
    struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\0\2\0', 0xee,
                     b'\xff\xff\xff', 1, SECTORS - 1)
    mbr[510:] = b'\x55\xaa'
    table = entries + bytes(align(len(entries), SECTOR) - len(entries))
    return bytes(mbr) + header(1, SECTORS - 1, 2) + table, table + header(SECTORS - 1, 1, SECTORS - 3)


def make_loader(header, entries, payloads, out):
    content = {n: payloads[n] for n in ['fes1.fex', 'u-boot.fex', 'config.fex', 'board.fex']}
    blob = bytearray((len(content) + 1) * 1024)
    blob[:1024] = header
    struct.pack_into('<I', blob, 60, len(content))
    for i, (name, b) in enumerate(content.items()):
        offset = align(len(blob), 1024)
        blob.extend(bytes(offset - len(blob)))
        entry = bytearray(1024)
        entry[:292] = entries[name][0][:292]
        stored = align(len(b), 16)
        struct.pack_into('<IIIII', entry, 292, stored, 0, len(b), 0, offset)
        blob[(i+1)*1024:(i+2)*1024] = entry
        blob.extend(b + bytes(stored-len(b)))
    blob.extend(bytes(align(len(blob), 1024)-len(blob)))
    struct.pack_into('<I', blob, 24, len(blob))
    if out.exists():
        assert out.read_bytes() == blob, 'Existing loader differs from original Tina components'
    else:
        out.write_bytes(blob)
    return {'sha256': digest(blob), 'bytes': len(blob), 'partitions': 0,
            'ram_only': True, 'sdmmc_logical_offset_sectors': 40960,
            'files': {n: {'bytes': len(b), 'sha256': digest(b)} for n, b in content.items()},
            'change': 'Container extraction only; all Tina components remain byte-for-byte unchanged.'}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--revision', type=int, choices=[6, 7, 8, 9, 10, 11], default=6)
    ap.add_argument('--legacy-debug-image', action='store_true',
                    help='Acknowledge persistent Tina Boot0/U-Boot; never a final release')
    args = ap.parse_args()
    if not args.legacy_debug_image:
        ap.error('This packer embeds Tina Boot0/U-Boot in the raw image and does '
                 'not meet the independent-boot requirement. Historical debug '
                 'reproduction requires --legacy-debug-image.')
    root = args.root.resolve()
    assert root == Path('/home/ubuntu/t736-mainline')
    output = root / 'output'
    raw = output / f't736-armbian-v{args.revision}.raw.img'
    loader = output / 't736-tina-fes-loader.img'
    assert not raw.exists()
    ref = output / 'validated-armbian-v2-reference.img'
    h, entries = read_package(ref)
    payloads = {}
    with ref.open('rb') as f:
        for name in list(EXPECTED) + ['fes1.fex', 'config.fex', 'board.fex']:
            _, offset, size = entries[name]
            f.seek(offset)
            payloads[name] = f.read(size)
    for name, expected in EXPECTED.items():
        assert digest(payloads[name]) == expected, name
    rootfs = output / f'rootfs-armbian-raw-v{args.revision}.ext4'
    assert rootfs.stat().st_size == LAYOUT[3][2] * SECTOR
    subprocess.run(['e2fsck', '-fn', str(rootfs)], check=True)
    primary, backup = gpt()
    regions = [('primary-gpt', 0, primary),
        ('vendor-boot0-user-copy', 16, payloads['boot0_sdcard.fex']),
        ('vendor-boot-package', 24576, payloads['boot_package.fex']),
        ('vendor-boot-package-backup', 32800, payloads['boot_package.fex']),
        ('vendor-gpt-mirror', 40960, primary),
        ('boot-resource', 73728, (output / 'raw-v6-boot-resource.fex').read_bytes()),
        ('env', 139264, gzip.decompress((output / 't736-current-env.gz').read_bytes())),
        ('boot', 172032, (output / 'boot-uboot-probe-v3.img').read_bytes()),
        ('backup-gpt', SECTORS - 3, backup)]
    assert len(regions[5][2]) <= LAYOUT[0][2] * SECTOR
    assert len(regions[6][2]) == LAYOUT[1][2] * SECTOR
    assert regions[7][2][:8] == b'ANDROID!' and len(regions[7][2]) < LAYOUT[2][2]*SECTOR
    records = []
    with raw.open('xb') as f:
        f.truncate(SECTORS * SECTOR)
        for name, sector, b in regions:
            f.seek(sector * SECTOR)
            f.write(b)
            records.append({'name': name, 'sector': sector, 'bytes': len(b), 'sha256': digest(b)})
        f.seek(LAYOUT[3][1] * SECTOR)
        with rootfs.open('rb') as source:
            while b := source.read(8 << 20):
                if any(b):
                    f.write(b)
                else:
                    f.seek(len(b), 1)
    records.append({'name': 'rootfs', 'sector': LAYOUT[3][1], 'bytes': rootfs.stat().st_size, 'sha256': file_digest(rootfs)})
    subprocess.run(['sfdisk', '--verify', str(raw)], check=True)
    subprocess.run(['sfdisk', '--json', str(raw)], check=True)
    # Check every written region independently, including the large rootfs.
    with raw.open('rb') as f:
        for record in records:
            f.seek(record['sector'] * SECTOR)
            h = hashlib.sha256()
            remaining = record['bytes']
            while remaining:
                b = f.read(min(8 << 20, remaining))
                assert b
                remaining -= len(b)
                h.update(b)
            assert h.hexdigest() == record['sha256'], record['name']
    lm = make_loader(read_package(ref)[0], entries, payloads, loader)
    manifest = {'raw': {'path': raw.name, 'bytes': raw.stat().st_size,
        'sha256': file_digest(raw), 'physical_sector_zero': True, 'regions': records,
        'partitions': LAYOUT, 'kernel_package': f'6.18.63-{args.revision}',
        'boot_chain': 'vendor Boot0/BL31/OP-TEE/SCP/U-Boot2018 -> mainline U-Boot2026.10 -> Linux6.18.63 -> Armbian',
        'postboot': 'Relocate backup GPT to actual eMMC end using sfdisk; no claim of a secure whole-device erase.'},
        'loader': lm, 'hardware_validated': False,
        'release_eligible': False,
        'independent_boot_compliant': False,
        'purpose': 'legacy_debug_reproduction',
        'persistent_vendor_components': ['Boot0', 'BL31', 'OP-TEE', 'SCP', 'U-Boot 2018.07']}
    raw.with_suffix('.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
