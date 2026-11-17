#!/usr/bin/env python3
"""Check decoded frame bytes against a pinned software-reference JSON.

This checks content only. The caller must independently verify hardware identity,
IRQ completion, decoder exit/EOS and absence of driver/allocation errors.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--reference-sha256', required=True)
    parser.add_argument('--input', required=True, help='NV21 file, or - for a clean raw byte stream')
    parser.add_argument('--result', type=Path)
    args = parser.parse_args()
    raw_reference = args.reference.read_bytes()
    if hashlib.sha256(raw_reference).hexdigest() != args.reference_sha256:
        raise ValueError('Reference JSON hash mismatch')
    reference = json.loads(raw_reference)['software_reference']
    frame_bytes = reference['frame_bytes']
    expected = reference['frame_sha256']
    if (reference['format'] != 'NV21' or reference['frames'] != len(expected)
            or frame_bytes != reference['width'] * reference['height'] * 3 // 2
            or reference['bytes'] != frame_bytes * len(expected)):
        raise ValueError('Invalid reference metadata')
    source = sys.stdin.buffer if args.input == '-' else open(args.input, 'rb')
    whole = hashlib.sha256()
    observed = []
    lengths = []
    try:
        while True:
            frame = source.read(frame_bytes)
            if not frame:
                break
            whole.update(frame)
            lengths.append(len(frame))
            observed.append(hashlib.sha256(frame).hexdigest())
    finally:
        if source is not sys.stdin.buffer:
            source.close()
    matches = [i < len(observed) and observed[i] == digest for i, digest in enumerate(expected)]
    passed = (len(observed) == len(expected) and all(length == frame_bytes for length in lengths)
              and all(matches) and whole.hexdigest() == reference['sha256'])
    report = {'schema': 1, 'status': 'PASS_FRAME_BYTES_ONLY' if passed else 'CONTENT_MISMATCH',
              'reference_json_sha256': args.reference_sha256,
              'observed_frames': len(observed), 'expected_frames': len(expected),
              'observed_bytes': sum(lengths), 'expected_bytes': reference['bytes'],
              'matched_frames': sum(matches), 'per_frame_exact_match': matches,
              'observed_frame_sha256': observed, 'observed_sha256': whole.hexdigest(),
              'expected_sha256': reference['sha256'],
              'hardware_acceptance': 'Not assessed by this content-only tool.'}
    output = json.dumps(report, indent=2)+'\n'
    if args.result:
        with args.result.open('x', encoding='utf-8') as destination:
            destination.write(output)
    print(output, end='')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
