#!/usr/bin/env python3
"""Build an unvalidated RAM handoff payload; this tool does not flash Boot1."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from pack_boot_validation import align, checksum


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ['header_source', 'shim', 'uboot', 'dtb', 'output']:
        parser.add_argument('--' + key.replace('_', '-'), type=Path, required=True)
    args = parser.parse_args()
    header = args.header_source.read_bytes()[:1600]
    shim, uboot, dtb = [p.read_bytes() for p in [args.shim, args.uboot, args.dtb]]
    if header[4:12] != b'uboot\0\0\0' or len(header) != 1600:
        raise ValueError('Unexpected board spare header')
    if not shim or len(shim) > 0xf000 or not uboot or len(uboot) > 0x70000:
        raise ValueError('Code exceeds the fixed handoff layout')
    if dtb[:4] != b'\xd0\x0d\xfe\xed' or len(dtb) > 0x80000:
        raise ValueError('Invalid board firmware DTB')
    if struct.unpack_from('>I', dtb, 4)[0] != len(dtb):
        raise ValueError('DTB length mismatch')
    image = bytearray(align(0x80000 + len(dtb), 16384))
    image[:1600] = header
    image[0x1000:0x1000 + len(shim)] = shim
    image[0x10000:0x10000 + len(uboot)] = uboot
    image[0x80000:0x80000 + len(dtb)] = dtb
    struct.pack_into('<I', image, 0, 0xea000000 | ((0x1000 - 8) // 4))
    struct.pack_into('<IIII', image, 12, 0x5f0a6c39, 16384, len(image), len(image))
    image[28:36] = b'0.1\0\0\0\0\0'
    # private_uboot.h: spare_boot_data_head.dtb_offset, after the 48-byte control header.
    struct.pack_into('<I', image, 1264, 0x80000)
    image[12:16] = checksum(image)
    with args.output.open('xb') as stream:
        stream.write(image)
    manifest = {'status': 'Compiled RAM candidate; not hardware validated or installed as Boot1',
                'bytes': len(image), 'sha256': hashlib.sha256(image).hexdigest(),
                'load_address': '0x4a000000', 'entry_address': '0x4a001000',
                'vendor_dependencies': ['1600-byte board spare header data', 'board firmware DTB',
                                        'DDR initialization', 'BL31', 'SCP', 'OP-TEE'],
                'old_vendor_uboot_executable_included': False,
                'inputs': {key: {'path': str(getattr(args, key)),
                                'sha256': hashlib.sha256(getattr(args, key).read_bytes()).hexdigest()}
                           for key in ['header_source', 'shim', 'uboot', 'dtb']}}
    args.output.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest))


if __name__ == '__main__':
    main()
