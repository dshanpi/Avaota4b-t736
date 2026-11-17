#!/usr/bin/env python3
"""Prepare only the small VE400 candidate sources and diff; never build or deploy."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path


BASELINES = {
    'clk-t736-ve.c': 'a68e6cf5c2c0ea2c94c4c55722cf2e53c936f7de45b666a00d62cf93cbace7bd',
    't736-ve-power.c': '8902a8e487ed6109c0dd9aadb83b75bdeb4e3a7bf16406dc35a9243a4970bfc9',
}
TARGETS = {
    'clk-t736-ve.c': 'drivers/clk/clk-t736-ve.c',
    't736-ve-power.c': 'drivers/soc/sunxi/t736-ve-power.c',
}
REPLACEMENTS = {
    'clk-t736-ve.c': [
        ('Fixed PERI0_800M / 4, no PLL writes.', 'Fixed PERI0_800M / 2, no PLL writes.'),
        ('#define VE_RATE 200000000UL', '#define VE_RATE 400000000UL'),
        ('#define VE_FIXED ((2U << 24) | 3)', '#define VE_FIXED ((2U << 24) | 1)'),
    ],
    't736-ve-power.c': [
        ('clk_get_rate(p->clocks[7].clk) != 200000000',
         'clk_get_rate(p->clocks[7].clk) != 400000000'),
        ('VE_DEC PPU=ON, 200MHz,', 'VE_DEC PPU=ON, 400MHz,'),
    ],
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = ap.parse_args()
    root = args.root.resolve()
    out = root/'boards/candidates/media-v28-400mhz'
    patch = []
    files = []
    prepared = []
    for name, baseline in BASELINES.items():
        source = root/'boards'/name
        data = source.read_bytes()
        if sha(data) != baseline:
            ap.error(f'{name}: v27 baseline hash changed; review before preparing')
        before = data.decode('utf-8')
        after = before
        for old, new in REPLACEMENTS[name]:
            if after.count(old) != 1:
                ap.error(f'{name}: expected exactly one replacement: {old}')
            after = after.replace(old, new, 1)
        prepared.append((name, after.encode('utf-8')))
        patch.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                         'a/'+TARGETS[name], 'b/'+TARGETS[name]))
        files.append({'path': name, 'kernel_path': TARGETS[name],
                      'baseline_sha256': baseline, 'candidate_sha256': sha(after.encode())})
    out.mkdir(parents=True, exist_ok=True)
    for name, data in prepared:
        (out/name).write_bytes(data)
    patchfile = out/'media-v28-ve400.patch'
    patchfile.write_text(''.join(patch), encoding='utf-8', newline='\n')
    record = {
        'schema': 1, 'status': 'SOURCE_CANDIDATE_ONLY_BUILD_AND_HARDWARE_PENDING',
        'name': 'media-v28-400mhz', 'board_access': False, 'kernel_built': False,
        'baseline': 'accepted media-v27, 200MHz',
        'change': 'VE_DEC PERI0_800M divider4 to divider2; fixed-rate and power checks400MHz',
        'register': {'parent_mux': 2, 'old_divider_field': 3, 'new_divider_field': 1,
                     'old_hz': 200000000, 'new_hz': 400000000,
                     'old_enabled_value': '0x82000003', 'new_enabled_value': '0x82000001'},
        'preserved': ['locked PERI800 parent verification', 'busy mismatched enabled clock returns EBUSY',
                      'unrelated register bits and shared gate ownership', 'divider readback',
                      'exclusive reset and reset-held publication', 'decoder-only bypass masters6/7',
                      'PPU/reset timing and all DMA lifetime behavior'],
        'not_changed': ['PLL registers', 'voltage/regulators', 'DT', 'Cedar DMA driver',
                        'GPU/storage/DDR', 'accepted v27 source/output'],
        'deployment_prerequisites': ['Voltage source/readback review accepted by root',
                                     'Fresh boot with no differently configured enabled VE clock owner',
                                     'Explicit root decision to build after copy benchmark'],
        'warm_clock_note': 'An enabled inherited200MHz gate is rejected; candidate never retimes an owned live gate.',
        'files': files, 'patch_sha256': sha(patchfile.read_bytes()),
    }
    (out/'manifest.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'candidate': str(out), 'patch_sha256': record['patch_sha256'],
                      'status': record['status']}))


if __name__ == '__main__':
    main()
