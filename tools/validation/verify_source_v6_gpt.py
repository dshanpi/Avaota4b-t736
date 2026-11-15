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


def check(blob, offset, start_lba):
    header = bytearray(blob[offset:offset + 512])
    assert header[:8] == b'EFI PART'
    size, crc = struct.unpack_from('<II', header, 12)
    struct.pack_into('<I', header, 16, 0)
    assert zlib.crc32(header[:size]) == crc
    current, backup, first, last = struct.unpack_from('<QQQQ', header, 24)
    table_lba, count, entry_size, table_crc = struct.unpack_from('<QIII', header, 72)
    begin = (table_lba - start_lba) * 512
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
                        help='Original source-v6 raw image to compare partition entries against')
    parser.add_argument('--evidence-dir', type=Path,
                        default=Path(__file__).resolve().parents[2] / 'evidence',
                        help='Directory containing source-v6-gpt-readback.log (default: project/evidence)')
    args = parser.parse_args()
    evidence = args.evidence_dir
    text = (evidence / 'source-v6-gpt-readback.log').read_text(encoding='utf-8').replace('\r', '')
    data = {}
    for name in ['primary', 'mirror', 'tail']:
        matches = re.findall(r'\nGPT_' + name + r'_BEGIN\n([A-Za-z0-9+/=\n]+)GPT_' + name + r'_END', text)
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
    result = dict(primary=p, secondary=s, vendor_mirror_identical=True,
                  all_partition_definitions_unchanged=True)
    for name, blob in data.items():
        (evidence / f'source-v6-gpt-after-{name}.bin').write_bytes(blob)
    (evidence / 'source-v6-gpt-verification.json').write_text(
        json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
