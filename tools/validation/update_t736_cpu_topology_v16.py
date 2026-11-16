#!/usr/bin/python3
"""Guarded, fixed-v16 CPU-topology DT update. Default is read-only check.

Only the fixed DTB and its private backup are written. No reboot, clock,
kernel, bootloader, boot command or other boot-file change is performed.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import stat
import tempfile

RELEASE = '6.18.63-t736-storage-v16'
MODEL = b'T736 custom board mainline bring-up\0'
TARGET = Path('/boot/dtb/allwinner/t736-mainline-bringup.dtb')
BACKUP_DIR = Path('/boot/t736-cpu-topology-v16-backup')
BACKUP = BACKUP_DIR / 'original.dtb'
MANIFEST = BACKUP_DIR / 'manifest.json'
OLD_SHA = '2ef0f4b2c0870fac80a6f66d0d6cb31b0cd795b5d69d89b31207e3d6ea8592b5'
NEW_SHA = '6124591aab1cfc02b287a88872835b20a6d2dac70b3e7cd2b93bb9af36d3e82b'
NEW_BYTES = 11928


def require(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def fsync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def regular(path):
    require(not path.is_symlink() and stat.S_ISREG(path.stat().st_mode),
            'Expected a regular, non-symlink file: ' + str(path))


def expected_manifest():
    return {'schema': 1, 'kernel_release': RELEASE, 'target': str(TARGET),
            'original_sha256': OLD_SHA, 'candidate_sha256': NEW_SHA,
            'candidate_bytes': NEW_BYTES, 'scope': 'CPU topology/capacity DT only'}


def checked_backup():
    require(BACKUP_DIR.is_dir() and not BACKUP_DIR.is_symlink(), 'Missing/invalid backup directory')
    regular(BACKUP)
    regular(MANIFEST)
    data = BACKUP.read_bytes()
    require(sha(data) == OLD_SHA, 'Original DTB backup SHA mismatch')
    require(json.loads(MANIFEST.read_text()) == expected_manifest(), 'Backup manifest mismatch')
    return data


def save_backup(data):
    if BACKUP_DIR.exists() or BACKUP_DIR.is_symlink():
        require(checked_backup() == data, 'Existing backup differs from the original DTB')
        return
    BACKUP_DIR.mkdir(mode=0o700)
    for path, payload in ((BACKUP, data),
                          (MANIFEST, (json.dumps(expected_manifest(), indent=2) + '\n').encode())):
        with path.open('xb') as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
    fsync_dir(BACKUP_DIR)
    fsync_dir(BACKUP_DIR.parent)
    require(checked_backup() == data, 'Backup readback failed')


def replace_dtb(data, old_stat, expected_sha):
    current = TARGET.stat()
    require((current.st_dev, current.st_ino, current.st_mtime_ns, current.st_size) ==
            (old_stat.st_dev, old_stat.st_ino, old_stat.st_mtime_ns, old_stat.st_size),
            'Target changed during validation')
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix='.t736-cpu-topology-', dir=TARGET.parent,
                                         delete=False) as stream:
            temporary = Path(stream.name)
            os.fchmod(stream.fileno(), stat.S_IMODE(old_stat.st_mode))
            os.fchown(stream.fileno(), old_stat.st_uid, old_stat.st_gid)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        require(sha(temporary.read_bytes()) == expected_sha, 'Temporary DTB readback failed')
        os.replace(temporary, TARGET)
        temporary = None
        fsync_dir(TARGET.parent)
        require(sha(TARGET.read_bytes()) == expected_sha, 'Installed DTB readback failed')
    finally:
        if temporary is not None:
            temporary.unlink()


def run(args):
    require(platform.release() == RELEASE, 'Running kernel must be the fixed storage-v16')
    require(Path('/proc/device-tree/model').read_bytes() == MODEL, 'Unexpected board model')
    require(TARGET.parent.resolve() == TARGET.parent, 'Unexpected symlink in target directory')
    regular(TARGET)
    if args.apply or args.rollback:
        require(os.geteuid() == 0, 'Apply/rollback requires root')
    with TARGET.open('rb') as stream:
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX if args.apply or args.rollback else fcntl.LOCK_SH)
        old_stat = os.fstat(stream.fileno())
        current = stream.read()
        current_sha = sha(current)
        require(current_sha in (OLD_SHA, NEW_SHA), 'Active boot DTB does not match either pinned state')
        result = {'kernel_release': RELEASE, 'target': str(TARGET), 'before_sha256': current_sha,
                  'backup': str(BACKUP), 'reboot_performed': False, 'other_boot_files_changed': False,
                  'runtime_capacity_validated': False}
        if args.rollback:
            original = checked_backup()
            if current_sha == OLD_SHA:
                return dict(result, status='already-original', after_sha256=OLD_SHA)
            replace_dtb(original, old_stat, OLD_SHA)
            return dict(result, status='rolled-back-reboot-required', after_sha256=OLD_SHA)
        regular(args.candidate)
        candidate = args.candidate.read_bytes()
        require(len(candidate) == NEW_BYTES and sha(candidate) == NEW_SHA, 'Candidate size/SHA mismatch')
        if current_sha == NEW_SHA:
            checked_backup()
            return dict(result, status='already-staged', after_sha256=NEW_SHA)
        if not args.apply:
            if BACKUP_DIR.exists() or BACKUP_DIR.is_symlink():
                checked_backup()
            return dict(result, status='check-ready', candidate_sha256=NEW_SHA,
                        backup_present=BACKUP_DIR.exists(), after_sha256=OLD_SHA)
        # First mutation always starts with the exact known original bytes.
        require(current_sha == OLD_SHA, 'Apply requires the fixed original DTB')
        save_backup(current)
        replace_dtb(candidate, old_stat, NEW_SHA)
        return dict(result, status='applied-reboot-required', after_sha256=NEW_SHA)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, default=Path('/tmp/t736-cpu-topology-candidate.dtb'))
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--apply', action='store_true')
    mode.add_argument('--rollback', action='store_true')
    args = parser.parse_args()
    try:
        result = run(args)
    except (OSError, ValueError, RuntimeError) as error:
        print(json.dumps({'status': 'rejected-or-failed', 'error': str(error), 'reboot_performed': False}))
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
