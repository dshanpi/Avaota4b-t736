"""Fault injection only: never invokes package installation, mounts or devices."""
import io
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

import update_t736_storage_v24 as update


class TransactionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='t736-v24-offline-')
        self.root = Path(self.temp.name)
        self.directory = self.root / 'backup'
        self.directory.mkdir()
        (self.directory / 'blobs').mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def backup_file(self, path):
        info = update.object_info(path)
        update.save(self.directory / 'blobs' / info['sha256'], path.read_bytes())
        return info

    def test_archive_escape_and_hardlink_rejected(self):
        for name, kind in (('../outside', tarfile.REGTYPE), ('/outside', tarfile.REGTYPE), ('inside', tarfile.LNKTYPE)):
            stream = io.BytesIO()
            with tarfile.open(fileobj=stream, mode='w') as archive:
                info = tarfile.TarInfo(name)
                info.type = kind
                archive.addfile(info, io.BytesIO())
            with self.subTest(name=name, kind=kind):
                with self.assertRaises(RuntimeError):
                    update.archive_manifest(stream.getvalue())

    def test_command_timeout_kills_descendant_before_returning(self):
        pidfile = self.root / 'descendant.pid'
        child = ('import subprocess,sys,time; from pathlib import Path; '
                 'p=subprocess.Popen([sys.executable,"-c","import time;time.sleep(60)"]); '
                 'Path(sys.argv[1]).write_text(str(p.pid)); time.sleep(60)')
        with self.assertRaises(subprocess.TimeoutExpired):
            update.run([sys.executable, '-c', child, str(pidfile)], timeout=0.5)
        self.assertTrue(pidfile.exists(), 'Synthetic descendant did not start')
        status = Path('/proc') / pidfile.read_text() / 'stat'
        if status.exists():
            self.assertEqual(status.read_text().split(') ', 1)[1].split()[0], 'Z',
                             'Descendant still running after timeout cleanup')

    def test_atomic_replace_does_not_follow_previous_symlink(self):
        payload = self.root / 'old-kernel'
        payload.write_bytes(b'old kernel stays intact')
        selected = self.root / 'alias'
        selected.symlink_to(payload.name)
        candidate = self.root / 'new-dtb'
        candidate.write_bytes(b'candidate DTB')
        info = self.backup_file(candidate)
        update.replace_object(selected, info, self.directory)
        self.assertFalse(selected.is_symlink())
        self.assertEqual(selected.read_bytes(), b'candidate DTB')
        self.assertEqual(payload.read_bytes(), b'old kernel stays intact')

    def test_corrupt_backup_rejects_before_any_restoration(self):
        old = self.root / 'old-kernel'
        old.write_bytes(b'known old kernel')
        before = {str(old): self.backup_file(old)}
        blob = self.directory / 'blobs' / before[str(old)]['sha256']
        blob.write_bytes(b'corrupt')
        old.write_bytes(b'current kernel')
        with mock.patch.object(update, 'replace_object') as replace:
            with self.assertRaisesRegex(RuntimeError, 'backup is corrupt'):
                update.restore(self.directory, before)
            replace.assert_not_called()
        self.assertEqual(old.read_bytes(), b'current kernel')

    def test_restore_preserves_candidate_and_restores_old_payload_and_alias(self):
        old = self.root / 'old-kernel'
        old.write_bytes(b'known old kernel')
        new = self.root / 'new-kernel'
        new.write_bytes(b'candidate kernel')
        alias = self.root / 'alias'
        alias.symlink_to(old.name)
        before = {str(old): self.backup_file(old), str(alias): update.object_info(alias)}
        old.write_bytes(b'unexpected old-file drift')
        alias.unlink()
        alias.symlink_to(new.name)
        with mock.patch.object(update, 'baseline_paths', return_value=[old, alias]):
            update.restore(self.directory, before)
        self.assertEqual(update.snapshot([old, alias]), before)
        self.assertEqual(new.read_bytes(), b'candidate kernel')

    def test_missing_bindings_cannot_apply(self):
        with mock.patch.dict(update.BINDINGS, {'kernel_sha256': None}):
            with mock.patch.object(update.sys, 'argv', ['updater', '--apply']):
                with mock.patch.object(update, 'device_check') as device:
                    self.assertEqual(update.main(), 3)
                    device.assert_not_called()

    def test_wrong_baseline_topology_rejected_before_candidate_install(self):
        dtb = self.root / 'baseline.dtb'
        dtb.write_bytes(b'old topology without the accepted capacity map')
        commands = mock.Mock(side_effect=[b'', b'install ok installed 6.18.63-16'])
        accepted = hashlib.sha256(b'accepted capacity topology').hexdigest()
        with mock.patch.object(update, 'identity_check'), mock.patch.object(update, 'run', commands), \
                mock.patch.object(update, 'ALIASES', {}), mock.patch.object(update, 'DTB', dtb), \
                mock.patch.dict(update.BINDINGS, {'baseline_dtb_sha256': accepted}):
            with self.assertRaisesRegex(RuntimeError, 'explicitly accepted v16 topology'):
                update.device_check()
        self.assertEqual(dtb.read_bytes(), b'old topology without the accepted capacity map')

    def test_validated_running_v23_is_allowed_without_changing_v16_fallback(self):
        previous_hashes = {}
        for stem in ('vmlinuz', 'config', 'System.map'):
            path = self.root / (stem + '-' + update.PREVIOUS)
            path.write_bytes(('validated v23 ' + stem).encode())
            previous_hashes[stem] = update.sha(path)
        dtb = self.root / 'v23-package.dtb'
        dtb.write_bytes(b'v23 package DT with accepted capacity')
        original_read_bytes = Path.read_bytes
        original_read_text = Path.read_text

        def read_bytes(path):
            return b'T736 custom board mainline bring-up\0' if str(path) == '/proc/device-tree/model' else original_read_bytes(path)

        def read_text(path, *args, **kwargs):
            return 'root=PARTUUID=' + update.ROOT_UUID if str(path) == '/proc/cmdline' else original_read_text(path, *args, **kwargs)

        def command(argv, **kwargs):
            if argv[0] == 'dpkg-query':
                self.assertEqual(argv[-1], 'linux-image-' + update.PREVIOUS)
                return b'install ok installed 6.18.63-23'
            if argv[0] == 'findmnt':
                return b'/dev/mmcblk0p4'
            if argv[0] == 'blkid':
                return update.ROOT_UUID.encode()
            raise AssertionError(argv)

        with mock.patch.object(update.os, 'geteuid', return_value=0), \
                mock.patch.object(update.os, 'uname', return_value=SimpleNamespace(machine='aarch64', release=update.PREVIOUS)), \
                mock.patch.object(update, 'BOOT', self.root), mock.patch.object(update, 'PREVIOUS_BOOT_SHA256', previous_hashes), \
                mock.patch.object(update, 'PREVIOUS_DTB', dtb), mock.patch.object(update, 'PREVIOUS_DTB_SHA256', update.sha(dtb)), \
                mock.patch.object(update, 'run', side_effect=command), \
                mock.patch.object(Path, 'read_bytes', new=read_bytes), mock.patch.object(Path, 'read_text', new=read_text):
            update.identity_check()
            self.assertEqual(update.OLD, '6.18.63-t736-storage-v16')
            (self.root / ('vmlinuz-' + update.PREVIOUS)).write_bytes(b'changed v23 kernel')
            with self.assertRaisesRegex(RuntimeError, 'Validated running v23 boot payload changed'):
                update.identity_check()

    def test_check_reports_running_v23_and_v16_fallback_separately(self):
        argv = ['updater', '--kernel', 'candidate.deb', '--pvr', 'candidate.ko']
        with mock.patch.object(update.sys, 'argv', argv), \
                mock.patch.object(update, 'pending', return_value=[]), \
                mock.patch.object(update, 'device_check'), mock.patch.object(update, 'inputs', return_value={}), \
                mock.patch.object(update, 'snapshot', return_value={}), \
                mock.patch.object(update.shutil, 'disk_usage', return_value=SimpleNamespace(free=2 * 1024 ** 3)), \
                mock.patch.object(update.os, 'uname', return_value=SimpleNamespace(release=update.PREVIOUS)), \
                mock.patch('builtins.print') as output:
            self.assertEqual(update.main(), 0)
        result = json.loads(output.call_args.args[0])
        self.assertEqual(result['running_release'], update.PREVIOUS)
        self.assertEqual(result['fallback_release'], update.OLD)
        self.assertFalse(result['write_attempted'])

    def test_final_dtb_mutation_rejected_even_when_kernel_bytes_match(self):
        raw = b'fixed raw candidate kernel'
        (self.root / ('vmlinuz-' + update.NEW)).write_bytes(gzip.compress(raw))
        dtb = self.root / 'candidate.dtb'
        good = b'accepted DMA and capacity DTB'
        dtb.write_bytes(b'unexpected DTB bytes')
        binding = {'raw_image_bytes': len(raw), 'raw_image_sha256': hashlib.sha256(raw).hexdigest(),
                   'candidate_dtb_bytes': len(good), 'candidate_dtb_sha256': hashlib.sha256(good).hexdigest()}
        with mock.patch.object(update, 'verify_payload'), mock.patch.object(update, 'BOOT', self.root), \
                mock.patch.object(update, 'NEW_DTB', dtb), mock.patch.dict(update.BINDINGS, binding):
            with self.assertRaisesRegex(RuntimeError, 'Installed DTB differs'):
                update.final_check({}, self.root / 'not-consumed.ko')

    def test_final_byte_failure_never_selects_candidate(self):
        kernel = self.directory / 'kernel.deb'
        kernel.write_bytes(b'fixture')
        pvr = self.directory / 'pvrsrvkm.ko'
        pvr.write_bytes(b'fixture module')
        selected = self.root / 'modules/extra/pvrsrvkm.ko'
        commands = []

        def command(argv, **unused):
            commands.append(argv[0])
            if argv[0] == 'dpkg-query':
                return ('install ok installed ' + update.VERSION).encode()
            return b''

        patches = {
            'backup': mock.Mock(return_value=self.directory),
            'suppress_hooks': mock.Mock(), 'inputs': mock.Mock(),
            'run': mock.Mock(side_effect=command), 'verify_payload': mock.Mock(),
            'snapshot': mock.Mock(return_value={}),
            'final_check': mock.Mock(side_effect=RuntimeError('synthetic final bytes mismatch')),
            'select_candidate': mock.Mock(), 'restore': mock.Mock(), 'PVR': selected,
        }
        with mock.patch.multiple(update, **patches):
            self.assertEqual(update.apply(kernel, pvr, {}, {}), 2)
        self.assertEqual(commands, ['dpkg', 'dpkg-query', 'depmod', 'update-initramfs'])
        patches['select_candidate'].assert_not_called()
        patches['restore'].assert_called_once_with(self.directory, {})
        result = json.loads((self.directory / 'failure.json').read_text())
        self.assertEqual(result['status'], 'UPDATE_FAILED_V16_RESTORED')

    def test_failure_journal_error_preserves_rollback_result(self):
        kernel = self.directory / 'kernel.deb'
        kernel.write_bytes(b'fixture')
        pvr = self.directory / 'pvrsrvkm.ko'
        pvr.write_bytes(b'fixture module')
        patches = {
            'backup': mock.Mock(return_value=self.directory),
            'suppress_hooks': mock.Mock(side_effect=RuntimeError('synthetic mount failure')),
            'restore': mock.Mock(), 'record': mock.Mock(side_effect=OSError('synthetic journal failure')),
        }
        with mock.patch.multiple(update, **patches), mock.patch('builtins.print') as output:
            self.assertEqual(update.apply(kernel, pvr, {}, {}), 2)
        patches['restore'].assert_called_once_with(self.directory, {})
        result = json.loads(output.call_args.args[0])
        self.assertEqual(result['status'], 'UPDATE_FAILED_V16_RESTORED')
        self.assertIn('journal_error', result)

    def test_unquiesced_child_never_races_rollback(self):
        kernel = self.directory / 'kernel.deb'
        pvr = self.directory / 'pvrsrvkm.ko'
        kernel.write_bytes(b'fixture')
        pvr.write_bytes(b'fixture module')
        patches = {
            'backup': mock.Mock(return_value=self.directory),
            'suppress_hooks': mock.Mock(), 'inputs': mock.Mock(),
            'run': mock.Mock(side_effect=update.ProcessGroupStillActive('synthetic D-state child')),
            'restore': mock.Mock(), 'select_candidate': mock.Mock(),
        }
        with mock.patch.multiple(update, **patches):
            self.assertEqual(update.apply(kernel, pvr, {}, {}), 2)
        patches['restore'].assert_not_called()
        patches['select_candidate'].assert_not_called()
        result = json.loads((self.directory / 'failure.json').read_text())
        self.assertEqual(result['status'], 'CRITICAL_SUBPROCESS_STILL_ACTIVE')
        self.assertFalse(result['rollback_attempted'])

    def test_failure_after_selection_attempt_restores_v16(self):
        kernel = self.directory / 'kernel.deb'
        kernel.write_bytes(b'fixture')
        pvr = self.directory / 'pvrsrvkm.ko'
        pvr.write_bytes(b'fixture module')

        def command(argv, **unused):
            return ('install ok installed ' + update.VERSION).encode() if argv[0] == 'dpkg-query' else b''

        patches = {
            'backup': mock.Mock(return_value=self.directory),
            'suppress_hooks': mock.Mock(), 'inputs': mock.Mock(),
            'run': mock.Mock(side_effect=command), 'verify_payload': mock.Mock(),
            'snapshot': mock.Mock(return_value={}), 'final_check': mock.Mock(return_value={}),
            'select_candidate': mock.Mock(side_effect=RuntimeError('synthetic alias fsync failure')),
            'restore': mock.Mock(), 'PVR': self.root / 'modules/extra/pvrsrvkm.ko',
        }
        with mock.patch.multiple(update, **patches):
            self.assertEqual(update.apply(kernel, pvr, {}, {}), 2)
        patches['select_candidate'].assert_called_once()
        patches['restore'].assert_called_once_with(self.directory, {})


if __name__ == '__main__':
    unittest.main()
