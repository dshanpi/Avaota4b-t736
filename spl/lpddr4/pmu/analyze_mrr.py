#!/usr/bin/env python3
"""Audit the parent's captured board MRR log; does not access hardware."""
from pathlib import Path
import hashlib
import json
import re


def main():
    root = Path(__file__).resolve().parents[3]
    source = root / 'evidence/lpddr4-mrr-hardware-20261114.log'
    destination = root / 'evidence/lpddr4-mrr-hardware-analysis-20261114.json'
    text = source.read_text(encoding='utf-8')
    regex = re.compile(r't736-lp4-mrr: pass=(\d+) ch=(\d+) rank=(\d+) '
                       r'mr=(\d+) rc=(-?\d+) raw=([0-9a-f]{8}) value=([0-9a-f]{2})')
    console, dmesg = [], []
    for line in text.splitlines():
        match = regex.search(line)
        if not match:
            continue
        fields = match.groups()
        record = dict(zip(('pass', 'channel', 'rank', 'mr', 'rc'), map(int, fields[:5])))
        record.update(raw=int(fields[5], 16), value=int(fields[6], 16))
        (dmesg if line.lstrip().startswith('[') else console).append(record)
    assert len(console) == 24 and len(dmesg) == 24, 'Missing or excess log records'
    assert console == dmesg, 'Console and dmesg are not identical transaction copies'
    expected_keys = [(p, ch, rank, mr) for p in range(2) for ch in range(2)
                     for rank in range(2) for mr in ((5, 6, 8) if p == 0 else (8, 6, 5))]
    keys = [(x['pass'], x['channel'], x['rank'], x['mr']) for x in console]
    assert keys == expected_keys and len(set(keys)) == 24, 'Incorrect transaction order'
    assert all(x['rc'] == 0 for x in console), 'Controller transaction failure'
    assert all(x['raw'] == x['value'] << (8 if x['channel'] == 0 else 0)
               for x in console), 'Unexpected byte-lane contents'
    values = {}
    for item in console:
        key = (item['channel'], item['rank'], item['mr'])
        if key in values:
            assert values[key] == item['value'], 'Reverse-pass value differs'
        values[key] = item['value']
    targets = []
    for channel in range(2):
        for rank in range(2):
            mr = {n: values[channel, rank, n] for n in (5, 6, 8)}
            assert len(set(mr.values())) == 3, 'No distinct MR-selection evidence'
            mr8 = mr[8]
            # Micron Rev.G table41: density is PER CHANNEL, width PER CHANNEL.
            density_table = {0: 2, 1: 3, 2: 4, 3: 6, 4: 8, 5: 12, 6: 16, 12: 1}
            density = density_table.get((mr8 >> 2) & 15)
            width = {0: 16, 1: 8}.get(mr8 >> 6)
            assert (mr8 & 3) == 0 and density == 16 and width == 16
            targets.append({'channel': channel, 'rank_requested': rank,
                            'mr5': f'0x{mr[5]:02x}', 'mr6': f'0x{mr[6]:02x}',
                            'mr8': f'0x{mr8:02x}', 's16_type_code': mr8 & 3,
                            'density_code': (mr8 >> 2) & 15,
                            'density_gibits_per_channel_rank': density,
                            'device_io_bits_per_channel': width})
    module_hashes = re.findall(r'([0-9a-f]{64})\s+/tmp/t736-transfer/lp4-mrr-20261114\.ko', text)
    assert len(module_hashes) == 1 and 'MRR_INSMOD_RC=0' in text
    evidence = {
        'schema_version': 1,
        'date': '2026-11-14',
        'scope': 'Independent offline analysis of parent-executed identity MRR under initialized Linux',
        'input_log': str(source.relative_to(root)),
        'input_log_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
        'analyzer_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'reported_module_sha256': module_hashes[0],
        'raw_record_count': 48,
        'unique_hardware_transactions': 24,
        'duplicate_dmesg_records_excluded': 24,
        'console_dmesg_identical': True,
        'expected_forward_reverse_order_confirmed': True,
        'all_return_codes_zero': True,
        'all_12_channel_rank_mr_values_match_reverse_pass': True,
        'observed_channel0_data_lane': 'raw bits15:8; all other bits zero in these samples',
        'observed_channel1_data_lane': 'raw bits7:0; all other bits zero in these samples',
        'targets': targets,
        'interpretation': {
            'manufacturer': 'MR5=0xff identifies Micron',
            'revision': 'MR6=0x07 is vendor-specific revision ID1=7; exact part/generation not identified',
            'mr8': '0x18: S16/16n prefetch, x16/channel, 16Gibits per channel (32Gibits for a dual-channel die)',
            'capacity_if_existing_topology_is_correct': {
                'channels': 2, 'ranks_per_channel': 2,
                'device_io_width_bits': 16, 'controller_channel_width_bits': 16,
                'formula': '2 channels * 2 ranks * 16 Gibits / 8 = 8 GiB',
                'bytes': 8589934592,
                'caution': 'Conditional consistency with existing topology, not an independent rank-routing/capacity test',
            },
        },
        'freshness_assessment': {
            'credible_command_dependent_readout': True,
            'basis': 'Three distinct correct MR values follow both forward and reversed request order for every requested channel/rank.',
            'excludes': ['one invariant stale latch throughout the run',
                         'a simple one-transaction-old response across these differing MR requests'],
            'does_not_establish': ['receive-valid/error/clear bit semantics or freshness for arbitrary requests',
                                   'distinct physical rank routing: both ranks report identical IDs',
                                   'independent physical channel routing: both channels report identical IDs',
                                   'memory alias-free capacity, training quality, or cold initialization'],
        },
        'sources': [
            {
                'title': 'Micron z19m_embedded_lpddr4_lpddr4x.pdf Rev.G 04/2022',
                'url': 'https://www.avnet.com/wcm/connect/3baa3889-1af3-47e9-ba83-fa7aad7c7f8e/micron-lpddr4-lpddr4x-sdram-datasheet.pdf',
                'tables': [35, 37, 41], 'printed_pages': [55, 56],
                'facts_used': 'MR5 ff=Micron; MR6 is vendor-specific; MR8 type00=S16, width00=x16/channel, density0110=16Gb/channel or32Gb/dual-channel die.',
            },
            {
                'repository': 'u-boot/u-boot',
                'commit': '6487c50b8a40f2668ab98a892b9300ad5c181f38',
                'path': 'board/gateworks/venice/spl.c',
                'blob': '2c303211d3280a1735f9aab9a1152d7bf5ee73d7',
                'license': 'GPL-2.0-or-later',
                'facts_used': 'Independent public LP4 readout recognizes ff=Micron, width code0=x16/code1=x8 and density code6=32Gb per dual-channel die.',
            },
        ],
        'vendor_binary_used_for_this_diagnostic': False,
        'dram_preinitialized_by_existing_legacy_boot_chain': True,
        'open_source_cold_initialization_demonstrated': False,
        'full_lp4_training_demonstrated': False,
        'normal_boot_deployable': False,
    }
    destination.write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    print(f'PASS: 24 transactions; identical dmesg copy excluded; {destination}')


if __name__ == '__main__':
    main()
