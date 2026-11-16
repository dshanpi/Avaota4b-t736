"""Verify post-install GPT bytes captured from the board's serial console.

This reads local files only. It writes decoded GPT snapshots and a JSON result
into the evidence directory; it never opens or modifies a board block device.
"""
import argparse
import base64
import hashlib
import json
import re
import struct
import zlib
from pathlib import Path


def file_sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def check(blob, offset, start_lba):
    header = bytearray(blob[offset:offset + 512])
    assert len(header) == 512 and header[:8] == b'EFI PART'
    size, crc = struct.unpack_from('<II', header, 12)
    assert 92 <= size <= 512
    struct.pack_into('<I', header, 16, 0)
    assert zlib.crc32(header[:size]) == crc
    current, backup, first, last = struct.unpack_from('<QQQQ', header, 24)
    table_lba, count, entry_size, table_crc = struct.unpack_from('<QIII', header, 72)
    begin = (table_lba - start_lba) * 512
    assert 0 <= begin and begin + count * entry_size <= len(blob)
    entries = blob[begin:begin + count * entry_size]
    assert count == 6 and entry_size == 128 and len(entries) == 768
    assert zlib.crc32(entries) == table_crc
    return dict(current_lba=current, backup_lba=backup, first_usable=first,
                last_usable=last, entries_lba=table_lba, partition_count=count,
                header_crc_valid=True, entries_crc_valid=True,
                entries_sha256=hashlib.sha256(entries).hexdigest()), entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--raw-image', type=Path, required=True,
                        help='Original source-v10 raw image to compare partition entries against')
    parser.add_argument('--evidence-dir', type=Path,
                        default=Path(__file__).resolve().parents[2] / 'evidence',
                        help='Directory containing source-v10-gpt-readback-20261116.log (default: project/evidence)')
    args = parser.parse_args()
    evidence = args.evidence_dir
    text = (evidence / 'source-v10-gpt-readback-20261116.log').read_text(encoding='utf-8').replace('\r', '')
    data = {}
    for name in ['primary', 'mirror', 'tail']:
        matches = re.findall(r'\nSOURCE_V10_GPT_' + name + r'_BEGIN\n([A-Za-z0-9+/=\n]+)SOURCE_V10_GPT_' + name + r'_END', text)
        assert len(matches) == 1, (name, len(matches))
        data[name] = base64.b64decode(matches[0])
    assert len(data['primary']) == len(data['mirror']) == 8192
    assert len(data['tail']) == 1536
    assert data['primary'] == data['mirror']
    assert data['primary'][510:512] == b'\x55\xaa'
    assert struct.unpack_from('<I', data['primary'], 458)[0] == 122142719

    p, pe = check(data['primary'], 512, 0)
    s, se = check(data['tail'], 1024, 122142717)
    assert p['current_lba'] == s['backup_lba'] == 1
    assert p['backup_lba'] == s['current_lba'] == 122142719
    assert p['first_usable'] == s['first_usable'] == 73728
    assert p['last_usable'] == s['last_usable'] == 122142716
    with args.raw_image.open('rb') as f:
        f.seek(1024)
        original = f.read(768)
    assert pe == se == original
    assert hashlib.sha256(original).hexdigest() == '750938222976776900b49d1782ee976c52db16fc42f1ab04ceeaffb3aaa4125f'
    manifest = json.loads(args.raw_image.with_suffix('.manifest.json').read_text())
    assert manifest['variant'] == 'v10'
    assert manifest['raw']['path'] == args.raw_image.name
    assert manifest['raw']['bytes'] == args.raw_image.stat().st_size == 8589934592
    assert manifest['cpu_boot_artifact_binding']['package_sha256'] == 'f2ea2157acc3b65b2cc7416dfdba39650e7daa6679693c558fbbdcc30ef9780a'
    raw_sha = file_sha(args.raw_image)
    assert raw_sha == manifest['raw']['sha256']
    result = dict(primary=p, secondary=s, vendor_mirror_identical=True,
                  all_partition_definitions_unchanged=True,
                  scope='GPT geometry only; not boot/runtime acceptance',
                  original_raw_sha256=raw_sha,
                  original_raw_cpu_package_sha256=manifest['cpu_boot_artifact_binding']['package_sha256'],
                  board_cpu_package_verified_by_this_tool=False,
                  final_image_cold_boot_validated=False)
    for name, blob in data.items():
        (evidence / f'source-v10-gpt-after-{name}-20261116.bin').write_bytes(blob)
    (evidence / 'source-v10-gpt-verification-20261116.json').write_text(
        json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
