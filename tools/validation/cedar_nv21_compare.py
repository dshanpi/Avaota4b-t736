#!/usr/bin/env python3
"""One bounded Cedar H.264 decode to NV21, compared against the fixed reference.

Run only on the prepared board. This is a 30-frame correctness check, not a
playback/zero-copy/encoding/browser acceptance test. Imported DMA is quarantined
until reboot in the first kernel candidate; budget repeated runs accordingly.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import time

FIXTURE_SHA = '470c8bb5cfb3159fb1566f115e452bf29791975686db6662fbc6a71a2ba6d267'
REFERENCE_SHA = 'cca7302cbb871683366faa34a4e0799493eb8fc63b095d66bdda54e7cd8f945b'
MANIFEST_SHA = '351878fa95940041f61495d30cf7914b298cb733f2a08b7d691a16e74f73fa81'
FRAME_BYTES = 320 * 240 * 3 // 2


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def irqs():
    result = {}
    for line in Path('/proc/interrupts').read_text(encoding='utf-8').splitlines():
        if 't736-cedar-wc' not in line:
            continue
        label, rest = line.split(':', 1)
        counts = []
        for word in rest.split():
            if not word.isdigit():
                break
            counts.append(int(word))
        result[label.strip()] = sum(counts)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--bundle', type=Path, default=Path('/opt/t736-cedar-wc-v1'))
    ap.add_argument('--fixture', type=Path, required=True)
    ap.add_argument('--reference', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True, help='New private results directory')
    ap.add_argument('--timeout', type=float, default=25)
    args = ap.parse_args()
    if not 1 <= args.timeout <= 60:
        ap.error('timeout must be between 1 and 60 seconds')
    if args.output.exists():
        ap.error('output directory must not already exist')
    args.bundle = args.bundle.resolve()
    require(sha(args.fixture) == FIXTURE_SHA, 'Unexpected compressed fixture')
    require(sha(args.reference) == REFERENCE_SHA, 'Unexpected NV21 software reference')
    require(sha(args.bundle/'manifest.json') == MANIFEST_SHA, 'Unexpected bundle manifest')
    manifest = json.loads((args.bundle/'manifest.json').read_text(encoding='utf-8'))
    require(manifest['kind'] == 't736-cedar-wc-v1', 'Unexpected bundle kind')
    for item in manifest['files']:
        path = (args.bundle/item['path']).resolve()
        require(args.bundle in path.parents, 'Invalid manifest path')
        require(path.stat().st_size == item['bytes'] and sha(path) == item['sha256'], str(path))
    for device in ('/dev/cedar_dev', '/dev/dma_heap/t736-ve'):
        require(stat.S_ISCHR(Path(device).stat().st_mode), device)
    args.output.mkdir(parents=True, mode=0o700)
    decoded = args.output/'decoded.nv21'
    log = args.output/'decode.log'
    # The vendor display loop increments its frame number before the save test.
    # Save [1, 31), and set the finish cap above 30 to let EOS drain B frames.
    command = [str(args.bundle/'bin/cedar-env'), str(args.bundle/'bin/vdecoderdemo'),
               '-i', str(args.fixture.resolve()), '-codFmat', '1', '-o', str(decoded.resolve()),
               '-n', '31', '-ss', '1', '-sn', '30', '-outFmat', '5', '-decNum', '1']
    before = irqs()
    started = time.monotonic()
    timed_out = False
    with log.open('wb') as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            for sig in (signal.SIGTERM, signal.SIGKILL):
                if process.poll() is not None:
                    break
                try:
                    os.killpg(process.pid, sig)
                except ProcessLookupError:
                    # The process may exit between poll() and killpg().
                    pass
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass
    after = irqs()
    raw = decoded.read_bytes() if decoded.exists() else b''
    reference = args.reference.read_bytes()
    matches = [raw[i:i+FRAME_BYTES] == reference[i:i+FRAME_BYTES]
               for i in range(0, len(reference), FRAME_BYTES)]
    text = log.read_text(encoding='utf-8', errors='replace')
    identity = re.findall(r'T736_WC: version=(0x[0-9a-f]+) phy_offset=(0x[0-9a-f]+)', text)
    delta = sum(after.values()) - sum(before.values())
    complete = (process.returncode == 0 and not timed_out and len(raw) == len(reference)
                and all(matches) and bool(identity) and all(int(x[1], 16) == 0 for x in identity)
                and delta > 0 and 'first validated WC ordering barrier' in text)
    report = {
        'schema': 1, 'status': 'PASS_H264_NV21_30_FRAME_CORRECTNESS' if complete else 'NOT_ACCEPTED',
        'kernel': os.uname().release, 'command': command, 'elapsed_seconds': time.monotonic()-started,
        'process_pid': process.pid, 'returncode': process.returncode, 'timed_out': timed_out,
        'process_still_running': process.poll() is None,
        'fixture_sha256': FIXTURE_SHA, 'reference_sha256': REFERENCE_SHA,
        'decoded_bytes': len(raw), 'expected_bytes': len(reference),
        'decoded_sha256': hashlib.sha256(raw).hexdigest(),
        'frame_bytes': FRAME_BYTES, 'expected_frames': 30,
        'matched_frames': sum(matches), 'per_frame_exact_match': matches,
        'version_and_phy_offset': identity, 'irq_before': before, 'irq_after': after, 'irq_delta': delta,
        'decode_log_sha256': sha(log), 'bundle_manifest_sha256': sha(args.bundle/'manifest.json'),
        'limits': ['320x240 H264 High/B-frame single fixture only; no throughput claim.',
                   'No player, HDMI, encoding, zero-copy or browser acceptance.',
                   'Imported DMA remains pinned until reboot in this bring-up kernel.',
                   'Any mismatch/timeout/remaining process is a failure requiring diagnosis; no silent fallback.'],
    }
    (args.output/'result.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report))
    return 0 if complete else 1


if __name__ == '__main__':
    raise SystemExit(main())
