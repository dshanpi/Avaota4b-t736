#!/usr/bin/env python3
"""Compile the pinned LPDDR5 PMU sources for investigation, never deployment.

This does not implement T736 LPDDR4 support. Prebuilt PMU input and missing
firmware stubs are forbidden. Outputs stay in build/, outside release output/.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    if root != Path('/home/ubuntu/t736-mainline'):
        raise SystemExit('Run only in the isolated Linux build project')
    for variable in ('PMU_FW', 'BINMAN_ALLOW_MISSING', 'ALLOW_MISSING'):
        if os.environ.get(variable):
            raise SystemExit(f'{variable} is forbidden for this source-only probe')
    policy = json.loads((root / 'config/boot-policy.json').read_text())
    if policy['normal_boot']['tina_vendor_binaries_allowed'] is not False:
        raise SystemExit('Expected the user-confirmed no-Tina-binary policy')
    source = root / 'sources/audit/a733-pmu-source-2f5e45fa'
    pinned = json.loads((source / 'source-manifest.json').read_text())
    if pinned['commit'] != '2f5e45fa7993ce8a4a59b638e539a06d8454f823':
        raise SystemExit('Unexpected PMU revision')
    inputs = []
    for entry in pinned['files']:
        path = source / Path(entry['path']).name
        data = path.read_bytes()
        git_blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if git_blob != entry['sha']:
            raise SystemExit(f'Source hash mismatch: {path}')
        inputs.append({'path': entry['path'], 'git_blob': git_blob,
                       'sha256': hashlib.sha256(data).hexdigest()})
    work = root / 'build/pmu-source-probe'
    work.mkdir(parents=True, exist_ok=True)
    header = work / 'dram_sun60i_a733_pmu_fw.h'
    command = [sys.executable, str(source / 'build_pmu_fw.py'),
               '--cross-compile', 'arc-linux-gnu-', '--src-dir', str(source),
               '--out', str(header)]
    print(json.dumps({'command': command, 'source_commit': pinned['commit']}, indent=2), flush=True)
    subprocess.run(command, check=True)
    text = header.read_text()
    if 'SUN60I_A733_PMU_FW_MISSING' in text:
        raise SystemExit('Nonfunctional PMU stub rejected')
    match = re.search(r'sun60i_a733_pmu_imem\[(\d+)\]\s*=\s*\{([^}]+)\}', text, re.S)
    if not match:
        raise SystemExit('No compiled PMU array in generated header')
    values = [int(value.strip(), 16) for value in match[2].split(',') if value.strip()]
    if len(values) != int(match[1]) or not 128 <= len(values) <= 32768:
        raise SystemExit('Invalid PMU image bounds')
    raw = struct.pack(f'<{len(values)}H', *values)
    binary = work / 'lpddr5-pmu-compile-probe.bin'
    binary.write_bytes(raw)
    version = subprocess.check_output(['arc-linux-gnu-gcc', '--version'], text=True).splitlines()[0]
    evidence = {'source': pinned['repository'], 'commit': pinned['commit'],
                'compiler': version, 'command': command, 'inputs': inputs,
                'binary': str(binary.relative_to(root)), 'bytes': len(raw),
                'sha256': hashlib.sha256(raw).hexdigest(),
                'prebuilt_vendor_binary_input': False, 'missing_stub_used': False,
                'compile_pass': True, 'supported_by_source_documentation': 'LPDDR5',
                't736_lpddr4_implemented': False, 'hardware_tested': False,
                'deployable': False}
    (root / 'evidence/open-pmu-compile-probe.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps({key: evidence[key] for key in ('bytes', 'sha256', 'compile_pass', 'deployable')}))


if __name__ == '__main__':
    main()
