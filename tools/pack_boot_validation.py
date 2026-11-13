#!/usr/bin/env python3
"""Build a bounded IMAGEWTY boot probe from this board's recovery package.

This retains vendor Boot0/Boot1 for the validation stage. It is not a final
mainline Armbian image. Use LYNX partition mode, never full_erase.
"""
import argparse
import array
import hashlib
import json
import re
import struct
import sys
from pathlib import Path


def align(value, size):
    return (value + size - 1) // size * size


def checksum(data):
    words = array.array('I')
    words.frombytes(data + bytes((-len(data)) % 4))
    if sys.byteorder != 'little':
        words.byteswap()
    return struct.pack('<I', sum(words) & 0xffffffff)


def validate_sparse(data, partition_bytes):
    if len(data) < 28:
        raise ValueError('Truncated sparse header')
    magic, major, minor, header_size, chunk_size, block, blocks, chunks, crc = struct.unpack_from('<IHHHHIIII', data)
    if (magic, major, minor, header_size, chunk_size, block) != (0xed26ff3a, 1, 0, 28, 12, 4096):
        raise ValueError('Unsupported Android sparse format')
    if blocks * block != partition_bytes:
        raise ValueError('Rootfs expanded size must match the existing partition')
    offset, expanded = header_size, 0
    for _ in range(chunks):
        if offset + chunk_size > len(data):
            raise ValueError('Truncated sparse chunk')
        kind, reserved, count, size = struct.unpack_from('<HHII', data, offset)
        payload = {0xcac1: count * block, 0xcac2: 4, 0xcac3: 0}.get(kind)
        if payload is None or size != chunk_size + payload or offset + size > len(data):
            raise ValueError('Invalid sparse chunk')
        expanded += count
        offset += size
    if offset != len(data) or expanded != blocks:
        raise ValueError('Sparse lengths do not match')


def read_package(path):
    with path.open('rb') as source:
        header = source.read(1024)
        if header[:8] != b'IMAGEWTY' or struct.unpack_from('<I', header, 8)[0] != 0x300:
            raise ValueError('Expected unencrypted IMAGEWTY version 3')
        count = struct.unpack_from('<I', header, 60)[0]
        if not 1 <= count <= 256:
            raise ValueError('Invalid file count')
        files = {}
        for i in range(count):
            source.seek(1024 * (i + 1))
            entry = source.read(1024)
            name = entry[36:292].split(b'\0')[0].decode('ascii')
            size = struct.unpack_from('<I', entry, 300)[0]
            offset = struct.unpack_from('<I', entry, 308)[0]
            if name in files or offset < (count + 1) * 1024 or offset + size > path.stat().st_size:
                raise ValueError('Duplicate entry or invalid file bounds')
            files[name] = (entry, offset, size)
    return header, files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--recovery', type=Path, required=True)
    parser.add_argument('--boot', type=Path, required=True)
    parser.add_argument('--rootfs', type=Path, help='Optional Armbian Android sparse rootfs')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    header, entries = read_package(args.recovery)
    keep = ['sys_config.fex', 'config.fex', 'board.fex', 'sys_partition.fex',
            'boot0_sdcard.fex', 'u-boot.fex', 'fes1.fex', 'boot_package.fex',
            'sunxi_gpt.fex', 'sunxi_mbr.fex', 'boot.fex', 'Vboot.fex']
    if args.output.exists():
        raise ValueError('Output exists; choose a new candidate name')
    payloads = {}
    with args.recovery.open('rb') as source:
        for name in keep:
            _, offset, size = entries[name]
            source.seek(offset)
            payloads[name] = source.read(size)
    original_hashes = {name: hashlib.sha256(data).hexdigest() for name, data in payloads.items()}
    boot = args.boot.read_bytes()
    if boot[:8] != b'ANDROID!' or len(boot) > 196608 * 512:
        raise ValueError('Expected Android validation payload within existing boot partition')
    payloads['boot.fex'] = boot
    payloads['Vboot.fex'] = checksum(boot)
    selected = [b'boot.fex']
    if args.rootfs:
        rootfs = args.rootfs.read_bytes()
        validate_sparse(rootfs, 15239680 * 512)
        keep += ['rootfs.fex', 'Vrootfs.fex']
        payloads['rootfs.fex'] = rootfs
        # Vendor unsparse_direct_write sums the sparse transport bytes.
        payloads['Vrootfs.fex'] = checksum(rootfs)
        selected.append(b'rootfs.fex')
    partition = payloads['sys_partition.fex']
    partition = re.sub(rb'(?mi)^[ \t]*downloadfile[ \t]*=[ \t]*"([^"\r\n]+)"[^\r\n]*',
                       lambda match: match.group(0) if match.group(1) in selected else b'', partition)
    downloads = re.findall(rb'(?mi)^[ \t]*downloadfile[ \t]*=[ \t]*"([^"]+)"', partition)
    if downloads != selected:
        raise ValueError('Partition downloads do not match the selected payloads')
    payloads['sys_partition.fex'] = partition
    image = bytearray((len(keep) + 1) * 1024)
    image[:1024] = header
    struct.pack_into('<I', image, 60, len(keep))
    manifest = {'purpose': 'FES mainline validation with vendor Boot0/Boot1; not final Armbian firmware',
                'required_mode': 'partition', 'partition_downloads': [name.decode().removesuffix('.fex') for name in selected],
                'persistent_vendor_boot0_boot1_retained': True, 'files': []}
    for i, name in enumerate(keep):
        old, _, _ = entries[name]
        data = payloads[name]
        offset = align(len(image), 1024)
        image.extend(bytes(offset - len(image)))
        stored = align(len(data), 16)
        entry = bytearray(1024)
        entry[:292] = old[:292]
        struct.pack_into('<IIIII', entry, 292, stored, 0, len(data), 0, offset)
        image[(i + 1) * 1024:(i + 2) * 1024] = entry
        image.extend(data)
        image.extend(bytes(stored - len(data)))
        digest = hashlib.sha256(data).hexdigest()
        manifest['files'].append({'name': name, 'bytes': len(data), 'sha256': digest,
                                  'unchanged_from_recovery': digest == original_hashes.get(name)})
    image.extend(bytes(align(len(image), 1024) - len(image)))
    struct.pack_into('<I', image, 24, len(image))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    manifest['sha256'] = hashlib.sha256(image).hexdigest()
    manifest['bytes'] = len(image)
    args.output.with_suffix('.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'path': str(args.output), 'bytes': len(image), 'sha256': manifest['sha256']}))


if __name__ == '__main__':
    main()
