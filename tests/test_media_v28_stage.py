#!/usr/bin/env python3
"""Linux host-only checks of stage validation and rollback failure boundaries."""
import argparse
from contextlib import redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import types
from unittest.mock import patch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--updater', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--pvr', type=Path, required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('stage', args.updater)
    stage = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(stage)
    checks = []

    def check(name, ok):
        if not ok:
            raise AssertionError(name)
        checks.append(name)

    payload = stage.inputs(args.kernel, args.pvr)
    check('actual fixed package/control/payload/PVR validation', bool(payload))
    with tempfile.TemporaryDirectory(prefix='t736-stage-host-test-') as directory:
        root = Path(directory)
        for rc in (0, 1):
            marker = root / ('child-' + str(rc))
            child = 'import os,time;from pathlib import Path;Path(' + repr(str(marker)) + ').write_text(str(os.getpid()));time.sleep(120)'
            parent = ('import subprocess,sys,time;from pathlib import Path;'
                      'subprocess.Popen([sys.executable,"-c",' + repr(child) + '],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);'
                      '\nwhile not Path(' + repr(str(marker)) + ').exists():time.sleep(.001)\n'
                      'sys.exit(' + str(rc) + ')')
            try:
                stage.run([sys.executable, '-c', parent], timeout=10)
                raise AssertionError('surviving descendant was accepted')
            except RuntimeError as error:
                check('parent rc%d descendant rejected' % rc, 'live descendants' in str(error))
            pid = int(marker.read_text())
            process = Path('/proc') / str(pid) / 'stat'
            check('parent rc%d descendant stopped before rollback' % rc,
                  not process.exists() or process.read_text().rsplit(') ', 1)[1].split()[0] in ('Z', 'X'))

        try:
            stage.run([sys.executable, '-c', 'import time;time.sleep(120)'], timeout=.05)
            raise AssertionError('timeout accepted')
        except __import__('subprocess').TimeoutExpired:
            checks.append('timeout command stopped and original error propagated')

        outside = root / 'outside'
        outside.mkdir()
        (outside / 'sentinel').write_text('unchanged')
        link = root / 'saved-directory'
        link.symlink_to(outside, target_is_directory=True)
        item = {'type': 'directory', 'mode': 0o700, 'uid': os.getuid(), 'gid': os.getgid()}
        stage.replace_object(link, item, root)
        check('directory leaf symlink restored without following', link.is_dir() and not link.is_symlink())
        check('outside target untouched', (outside / 'sentinel').read_text() == 'unchanged')
        parentlink = root / 'unsafe-parent'
        parentlink.symlink_to(outside, target_is_directory=True)
        try:
            stage.replace_object(parentlink / 'child', item, root)
            raise AssertionError('symlink parent accepted')
        except RuntimeError as error:
            check('symlink parent rejected before write', 'symlink parent' in str(error) and not (outside / 'child').exists())

        backup_parent = root / 'backups'
        backup = backup_parent / 'saved'
        blobs = backup / 'blobs'
        blobs.mkdir(parents=True)
        os.chmod(backup, 0o700)
        boot = root / 'boot'
        boot.mkdir()
        kernel = boot / ('vmlinuz-' + stage.OLD)
        kernel.write_bytes(b'protected old kernel')
        alias = boot / 'vmlinuz-6.18.63-t736-armbian'
        alias.symlink_to(kernel.name)
        roots = [kernel, alias]
        before = stage.snapshot(roots)
        digest = before[str(kernel)]['sha256']
        (blobs / digest).write_bytes(kernel.read_bytes())
        (backup / 'before.json').write_text(json.dumps(before))
        (backup / 'protected-roots.json').write_text(json.dumps([str(p) for p in roots]))
        kernel.unlink()
        real_lstat = Path.lstat

        def journal_lstat(path, *a, **kw):
            if path == backup:
                return types.SimpleNamespace(st_mode=stat.S_IFDIR | 0o700, st_uid=0)
            return real_lstat(path, *a, **kw)

        # Only journal ownership is simulated on this unprivileged build host.
        with patch.object(stage, 'BACKUPS', backup_parent), patch.object(stage, 'BOOT', boot), \
             patch.object(stage, 'ALIASES', {alias: 'vmlinuz'}), \
             patch.object(stage, 'fixed_baseline_paths', lambda: roots), \
             patch.object(stage, 'baseline_paths', lambda: roots), \
             patch.object(stage, 'identity_check', lambda *a: None), patch.object(Path, 'lstat', journal_lstat):
            loaded = stage.rollback_check(backup)
            check('deleted protected boot file accepted for recovery', loaded == before)
            stage.restore(backup, loaded)
            check('deleted protected boot file restored exactly', stage.snapshot(roots) == before)

    unbound = dict(stage.BINDINGS, kernel_sha256=None)
    for argv in ([], ['--apply'], ['--rollback', '/not-a-backup'], ['--rollback', '/not-a-backup', '--apply']):
        with patch.object(stage, 'BINDINGS', unbound), patch.object(sys, 'argv', ['stage'] + argv), \
             patch.object(stage, 'device_check', side_effect=AssertionError('board access')), \
             patch.object(stage, 'rollback_check', side_effect=AssertionError('rollback access')), redirect_stdout(io.StringIO()):
            rc = stage.main()
        check('unbound blocks before board access: ' + repr(argv), rc == 3)
    print(json.dumps({'status': 'PASS_HOST_ONLY', 'board_access': False, 'checks': checks}, indent=2))


if __name__ == '__main__':
    main()
