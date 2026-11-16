#!/usr/bin/env python3
"""Apply a T736DEL1 copy/literal delta after verifying both SHA256 hashes."""
import bz2
import hashlib
import struct
import sys
from pathlib import Path


def apply(base, delta):
    if delta[:8] != b'T736DEL1' or len(delta) < 88:
        raise ValueError('Invalid delta header')
    old_size, new_size = struct.unpack_from('<QQ', delta, 8)
    old_hash, new_hash = delta[24:56], delta[56:88]
    if len(base) != old_size or hashlib.sha256(base).digest() != old_hash:
        raise ValueError('Base size/SHA256 mismatch')
    stream = bz2.decompress(delta[88:])
    output = bytearray()
    pos = 0
    while pos < len(stream):
        op = stream[pos:pos + 1]
        pos += 1
        if op == b'C':
            offset, size = struct.unpack_from('<QQ', stream, pos)
            pos += 16
            if offset + size > len(base):
                raise ValueError('Copy beyond base')
            output.extend(base[offset:offset + size])
        elif op == b'D':
            size, = struct.unpack_from('<I', stream, pos)
            pos += 4
            if pos + size > len(stream):
                raise ValueError('Truncated literal')
            output.extend(stream[pos:pos + size])
            pos += size
        else:
            raise ValueError('Invalid delta opcode')
        if len(output) > new_size:
            raise ValueError('Output exceeds declared size')
    if len(output) != new_size or hashlib.sha256(output).digest() != new_hash:
        raise ValueError('Result size/SHA256 mismatch')
    return bytes(output)


def main():
    if len(sys.argv) != 4:
        raise SystemExit('usage: apply_binary_delta.py BASE DELTA OUTPUT')
    base, delta, output = map(Path, sys.argv[1:])
    result = apply(base.read_bytes(), delta.read_bytes())
    if output.exists():
        if output.read_bytes() != result:
            raise SystemExit('Refusing to replace a different output file')
    else:
        with output.open('xb') as target:
            target.write(result)
    print(hashlib.sha256(result).hexdigest() + '  ' + str(output))


if __name__ == '__main__':
    main()
