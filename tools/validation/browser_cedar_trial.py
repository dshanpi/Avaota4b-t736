#!/usr/bin/env python3
"""Bounded board test through the same preconnected client used by Firefox.

Input consists of <uint32 bytes,int64 pts_us,int64 dts_us> + Annex-B AU.
No raw video is saved: every visible I420 frame is hashed against a reference.
Run as the desktop user, with the isolated client preloaded before exec.
"""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import time

p = argparse.ArgumentParser()
p.add_argument('--records', required=True, type=Path)
p.add_argument('--reference', required=True, type=Path)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--repeat', type=int, default=1, choices=(1, 2))
a = p.parse_args()
assert not a.output.exists()

class Frame(C.Structure):
    _fields_ = [('width', C.c_uint32), ('height', C.c_uint32),
                ('bytes_written', C.c_uint32), ('color_range', C.c_uint32),
                ('colorspace', C.c_uint32), ('pts_us', C.c_int64)]

lib = C.CDLL(None)
lib.t736_client_open.argtypes = (C.c_int, C.c_uint, C.c_uint)
lib.t736_client_push.argtypes = (C.c_int, C.c_void_p, C.c_size_t, C.c_int64, C.c_int64)
lib.t736_client_pull.argtypes = (C.c_int, C.c_void_p, C.c_size_t, C.POINTER(Frame), C.c_uint)
expected = [line.rsplit(',', 1)[1].strip() for line in a.reference.read_text().splitlines()
            if line and not line.startswith('#')]
records = []
with a.records.open('rb') as f:
    while head := f.read(20):
        assert len(head) == 20
        size, pts, dts = struct.unpack('<Iqq', head)
        data = f.read(size)
        assert len(data) == size
        records.append((data, pts, dts))
assert len(records) == len(expected) == 180

out = {'schema': 1, 'kind': 'browser-cedar-client-hardware-trial',
       'codec': 'HEVC Main 8-bit', 'width': 1920, 'height': 1080,
       'input_rate': 60, 'expected_frames': len(expected), 'runs': []}
session = 0
try:
    assert lib.t736_client_available() == 1, 'Pre-sandbox connection not available'
    for run in range(a.repeat):
        hashes, times = [], []
        start = time.monotonic()
        session = lib.t736_client_open(2, 1920, 1080)
        assert session > 0, f'open returned {session}'
        buffer = C.create_string_buffer(1920 * 1080 * 3 // 2)
        meta = Frame()

        def pull(wait):
            rc = lib.t736_client_pull(session, buffer, len(buffer), C.byref(meta), wait)
            assert rc >= 0, f'pull returned {rc}'
            if rc == 1:
                assert (meta.width, meta.height, meta.bytes_written) == (1920, 1080, len(buffer))
                hashes.append(hashlib.md5(buffer.raw).hexdigest())
                times.append(meta.pts_us)
            return rc

        for data, pts, dts in records:
            deadline = time.monotonic() + 5
            while True:
                rc = lib.t736_client_push(session, data, len(data), pts, dts)
                if not rc: break
                assert rc == -11 and time.monotonic() < deadline, f'push returned {rc}'
                pull(20)
            while pull(0) == 1: pass
        assert lib.t736_client_drain(session) == 0
        deadline = time.monotonic() + 10
        while True:
            rc = pull(1000)
            if rc == 2: break
            assert time.monotonic() < deadline, 'EOS drain timed out'
        result = {'session': session, 'elapsed_seconds': time.monotonic() - start,
                  'frames': len(hashes), 'frame_md5': hashes, 'pts_us': times,
                  'all_frames_exact': hashes == expected,
                  'pts_monotonic': times == sorted(times)}
        out['runs'].append(result)
        assert result['all_frames_exact'], 'Hardware frame output differs from software reference'
        rc = lib.t736_client_close(session)
        session = 0
        assert rc == 0, f'close/flush returned {rc}'
    out['passed'] = True
except Exception as exc:
    out['passed'] = False
    out['error'] = repr(exc)
    raise
finally:
    if session > 0:
        out['close_after_error'] = lib.t736_client_close(session)
    a.output.write_text(json.dumps(out, indent=2) + '\n')
    print(json.dumps({k: v for k, v in out.items() if k != 'runs'}))
    for run in out['runs']:
        print(json.dumps({k: v for k, v in run.items() if k not in ('frame_md5', 'pts_us')}))
