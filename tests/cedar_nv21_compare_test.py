#!/usr/bin/env python3
"""Host-only acceptance-gate tests; every hardware/process boundary is mocked."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import signal
import subprocess
import tempfile
import types
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    'compare', Path(__file__).resolve().parents[1]/'tools/validation/cedar_nv21_compare.py')
COMPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPARE)


class AcceptanceGate(unittest.TestCase):
    def run_gate(self, *, damage=False, irq=True, offset=0, barrier=True,
                 exit_status=0, timeout_race=False, remains_running=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture, reference = root/'input.h264', root/'reference.nv21'
            fixture.write_bytes(b'mocked compressed input')
            frames = bytes(range(256)) * (COMPARE.FRAME_BYTES * 30 // 256)
            reference.write_bytes(frames)
            manifest = root/'manifest.json'
            manifest.write_text('{"kind":"t736-cedar-wc-v1","files":[]}', encoding='utf-8')
            output = root/'results'
            process = types.SimpleNamespace(pid=12345, returncode=None)
            process.poll = lambda: process.returncode
            calls = []

            def wait(timeout):
                calls.append(timeout)
                if remains_running or timeout_race and len(calls) == 1:
                    raise subprocess.TimeoutExpired('mock', timeout)
                process.returncode = exit_status
                return exit_status

            process.wait = wait

            def popen(command, *, stdout, **kwargs):
                self.assertEqual(command[command.index('-ss')+1], '1')
                self.assertEqual(command[command.index('-sn')+1], '30')
                self.assertEqual(command[command.index('-n')+1], '31')
                self.assertTrue(kwargs['start_new_session'])
                (output/'decoded.nv21').write_bytes(frames[:-1] if damage else frames)
                text = f'T736_WC: version=0x1720 phy_offset=0x{offset:x}\n'
                if barrier:
                    text += 'T736_WC: first validated WC ordering barrier\n'
                stdout.write(text.encode())
                stdout.flush()
                return process

            arguments = ['compare', '--bundle', str(root), '--fixture', str(fixture),
                         '--reference', str(reference), '--output', str(output)]
            with contextlib.ExitStack() as stack:
                stack.enter_context(patch('sys.argv', arguments))
                for name, value in [('FIXTURE_SHA', COMPARE.sha(fixture)),
                                    ('REFERENCE_SHA', COMPARE.sha(reference)),
                                    ('MANIFEST_SHA', COMPARE.sha(manifest))]:
                    stack.enter_context(patch.object(COMPARE, name, value))
                stack.enter_context(patch.object(COMPARE, 'irqs', side_effect=[{'47': 0}, {'47': int(irq)}]))
                stack.enter_context(patch.object(COMPARE.stat, 'S_ISCHR', return_value=True))
                real_stat = Path.stat

                def stat(path, *args, **kwargs):
                    if str(path).replace('\\', '/') in ('/dev/cedar_dev', '/dev/dma_heap/t736-ve'):
                        return types.SimpleNamespace(st_mode=0)
                    return real_stat(path, *args, **kwargs)

                stack.enter_context(patch.object(Path, 'stat', stat))
                stack.enter_context(patch.object(COMPARE.subprocess, 'Popen', side_effect=popen))
                stack.enter_context(patch.object(COMPARE.os, 'uname', create=True,
                                                return_value=types.SimpleNamespace(release='MOCK-NOT-HARDWARE')))
                stack.enter_context(patch.object(COMPARE.signal, 'SIGKILL', 9, create=True))
                kill = stack.enter_context(patch.object(COMPARE.os, 'killpg', create=True,
                                                       side_effect=ProcessLookupError if timeout_race else None))
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                code = COMPARE.main()
            report = json.loads((output/'result.json').read_text(encoding='utf-8'))
            return code, report, kill.call_args_list

    def test_all_evidence_required(self):
        code, report, _ = self.run_gate()
        self.assertEqual(code, 0)
        self.assertEqual(report['matched_frames'], 30)
        self.assertEqual(report['status'], 'PASS_H264_NV21_30_FRAME_CORRECTNESS')
        for change in ({'damage': True}, {'irq': False}, {'offset': 0x40000000},
                       {'barrier': False}, {'exit_status': 1}):
            with self.subTest(change=change):
                code, report, _ = self.run_gate(**change)
                self.assertEqual(code, 1)
                self.assertEqual(report['status'], 'NOT_ACCEPTED')

    def test_timeout_exit_race_is_failure(self):
        code, report, calls = self.run_gate(timeout_race=True)
        self.assertEqual(code, 1)
        self.assertTrue(report['timed_out'])
        self.assertFalse(report['process_still_running'])
        self.assertEqual(len(calls), 1)

    def test_uninterruptible_process_is_reported(self):
        code, report, calls = self.run_gate(remains_running=True)
        self.assertEqual(code, 1)
        self.assertTrue(report['process_still_running'])
        self.assertEqual([c.args[1] for c in calls], [signal.SIGTERM, 9])


if __name__ == '__main__':
    unittest.main(verbosity=2)
