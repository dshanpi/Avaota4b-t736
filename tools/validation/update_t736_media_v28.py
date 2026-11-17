#!/usr/bin/env python3
"""Stage the isolated T736 media-v28 candidate; check is the default.

The v16 default selection and accepted v27 files are protected byte-for-byte.
Apply installs only the versioned candidate package/PVR/initrd in a private
mount namespace with package/initramfs post-update hooks suppressed. It never
selects the candidate, reboots, writes bootcmd/SPL/GPT, or accesses raw media.
Fixed reviewed artifact bindings are required before any board action.
"""
import argparse
import datetime
import fcntl
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import time

OLD = '6.18.63-t736-storage-v16'
NEW = '6.18.63-t736-media-v28'
VERSION = '6.18.63-28'
PREVIOUS = '6.18.63-t736-media-v27'
RUNNING_RELEASES = (OLD, PREVIOUS)
PREVIOUS_DTB = Path('/usr/lib/linux-image-' + PREVIOUS + '/allwinner/t736-gpu-candidate.dtb')
PREVIOUS_DTB_SHA256 = '45012aeb86d657fca568417e831067170d12e224ebe71720c1956989af6f3626'
ROOT_UUID = 'a0085546-4166-744a-a353-fca9272b8e48'
BOOT = Path('/boot')
ALIASES = {BOOT / (stem + '-6.18.63-t736-armbian'): stem for stem in ('vmlinuz', 'initrd.img')}
DTB = BOOT / 'dtb/allwinner/t736-mainline-bringup.dtb'
NEW_DTB = Path('/usr/lib/linux-image-' + NEW + '/allwinner/t736-gpu-candidate.dtb')
PVR = Path('/usr/lib/modules') / NEW / 'extra/pvrsrvkm.ko'
BACKUPS = Path('/var/backups/t736-media-v28')
# Fixed v16 default and unchanged v27/v28 DTB. Final combined DCDC2-readback/
# VE400 package, payload, control, Image and PVR bindings audited independently.
BINDINGS = {'baseline_dtb_sha256': '6124591aab1cfc02b287a88872835b20a6d2dac70b3e7cd2b93bb9af36d3e82b', 'candidate_dtb_bytes': 13012, 'candidate_dtb_sha256': '45012aeb86d657fca568417e831067170d12e224ebe71720c1956989af6f3626', 'control_manifest_sha256': 'bfe49f040393782a7337feeb847b8731cf84f6eb6ca775d5dd572070a9eaf080', 'kernel_bytes': 5614712, 'kernel_sha256': 'cd8d775be309264f617e4023408e6a4fa5a9610d5e3e397663ecc997fa410964', 'payload_manifest_sha256': '166d1c1e8382adfc3583267d8df9ba12da72eab122b91e12a2a848a26cdfd885', 'pvr_bytes': 1913808, 'pvr_sha256': '497898feb827a3f933c2c783d881d54e095654e0e42598ddb222ba19ab95f847', 'raw_image_bytes': 11800584, 'raw_image_sha256': 'df86defafd80a38163fee2b086d084a06a8eaab32f02dee94e3ec391088b019f'}
BASELINE_BOOT_SHA256 = {'vmlinuz': '0244c994eb6092e2d68f8ae4bef9347c9ce11507790b8eb351db1a5a8023ccee', 'config': 'c2a5adf5941401efa3a4b474fb43afae3183049ec6a5c61194c4f2aeedb6449e', 'System.map': 'ebdda5fb6f14eacb6775602f3cc245af86c87f4780d6d4a046c886d29d608ea5'}
PREVIOUS_BOOT_SHA256 = {'vmlinuz': 'ae1cdde51209b120c1800dd6c4bb5ae6000bd60d8ce808414fb4e732780e7a9e', 'config': 'cb3eab53d6fb243b620eb34ebbd3b6284aa3fde869106c5a147a65c8e91ccfac', 'System.map': 'a15c1a5c0b4e452231c6f59279ef23f9220c7def0cc589612e519ab3263625f3'}
PREVIOUS_PVR_SHA256 = '981d873348e061af66c030f21af1f050ccb7d4df156ac9abbfcfa7a885a6652d'
HOOKS = ('/etc/kernel', '/usr/share/kernel', '/etc/initramfs/post-update.d',
         '/etc/initramfs-tools/post-update.d')


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for part in iter(lambda: stream.read(1048576), b''):
            h.update(part)
    return h.hexdigest()


class ProcessGroupStillActive(RuntimeError):
    """Do not race rollback against a subprocess that has not exited."""


def wait_group_stopped(group, timeout=10):
    deadline = time.monotonic() + timeout
    while True:
        active = []
        for entry in Path('/proc').iterdir():
            if not entry.name.isdecimal():
                continue
            try:
                fields = (entry / 'stat').read_text().rsplit(') ', 1)[1].split()
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                continue
            if int(fields[2]) == group and fields[0] not in ('Z', 'X'):
                active.append(int(entry.name))
        if not active:
            return
        if time.monotonic() >= deadline:
            raise ProcessGroupStillActive('Killed subprocess group still active: ' + str(active))
        time.sleep(0.01)


def run(argv, timeout=1800):
    # dpkg and initramfs-tools spawn children. Stop their entire process group
    # before any rollback can begin, including when a descendant holds a pipe.
    process = subprocess.Popen([str(x) for x in argv], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               start_new_session=True)
    try:
        out, err = process.communicate(timeout=timeout)
    except BaseException:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except OSError as stop_error:
            raise ProcessGroupStillActive('Could not stop timed-out command group') from stop_error
        try:
            process.communicate(timeout=10)
        except subprocess.TimeoutExpired as stop_error:
            raise ProcessGroupStillActive('Killed subprocess did not close its pipes') from stop_error
        wait_group_stopped(process.pid)
        raise
    # A parent can exit after a descendant closes the captured pipes. Do not
    # mistake communicate() completing for the whole writer group stopping.
    try:
        wait_group_stopped(process.pid, timeout=0)
    except ProcessGroupStillActive:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except OSError as stop_error:
            raise ProcessGroupStillActive('Could not stop detached command descendants') from stop_error
        wait_group_stopped(process.pid)
        raise RuntimeError('Subprocess exited with live descendants; group stopped before rollback')
    require(process.returncode == 0, f'{argv[0]} rc={process.returncode}: {err.decode(errors="replace")}')
    return out


def syncdir(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def save(path, data, mode=0o600):
    with Path(path).open('xb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(path, mode)
    syncdir(Path(path).parent)


def record(path, value):
    save(path, (json.dumps(value, indent=2, sort_keys=True) + '\n').encode())


def pending():
    return [key for key, value in BINDINGS.items() if value is None]


def clean_name(name):
    while name.startswith('./'):
        name = name[2:]
    p = PurePosixPath(name)
    require(not p.is_absolute() and '..' not in p.parts, 'Archive path escapes root')
    return str(p)


def archive_manifest(data):
    result = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:*') as archive:
        for member in archive:
            name = clean_name(member.name)
            if member.isdir():
                continue
            require(name != '.' and name not in result, 'Duplicate/empty archive entry')
            require(member.isfile() or member.issym(), 'Unsupported archive entry type')
            item = {'mode': member.mode & 0o7777, 'type': 'file' if member.isfile() else 'symlink'}
            if member.isfile():
                item.update(sha256=hashlib.sha256(archive.extractfile(member).read()).hexdigest(), bytes=member.size)
            else:
                item['target'] = member.linkname
            result[name] = item
    return result


def manifest_sha(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def inputs(kernel, pvr):
    require(not pending(), 'Build hashes are not fixed: ' + ', '.join(pending()))
    for path, key in ((kernel, 'kernel'), (pvr, 'pvr')):
        require(path.is_file() and not path.is_symlink(), 'Input must be a regular non-symlink file')
        require(path.stat().st_size == BINDINGS[key + '_bytes'] and sha(path) == BINDINGS[key + '_sha256'],
                key + ' size/SHA mismatch')
    fields = run(['dpkg-deb', '-f', kernel, 'Package', 'Version', 'Architecture']).decode().splitlines()
    require(fields == ['Package: linux-image-' + NEW, 'Version: ' + VERSION, 'Architecture: arm64'],
            'Kernel package metadata mismatch')
    control = archive_manifest(run(['dpkg-deb', '--ctrl-tarfile', kernel]))
    payload = archive_manifest(run(['dpkg-deb', '--fsys-tarfile', kernel]))
    require(manifest_sha(control) == BINDINGS['control_manifest_sha256'], 'Package maintainer/control files changed')
    require(manifest_sha(payload) == BINDINGS['payload_manifest_sha256'], 'Package payload changed')
    candidate_dtb = payload.get(str(NEW_DTB).lstrip('/'), {})
    require(candidate_dtb.get('type') == 'file'
            and candidate_dtb.get('sha256') == BINDINGS['candidate_dtb_sha256']
            and candidate_dtb.get('bytes') == BINDINGS['candidate_dtb_bytes'],
            'Package DTB differs from the fixed final VE400/readback candidate')
    for name in payload:
        require(name in ('boot/vmlinuz-' + NEW, 'boot/config-' + NEW, 'boot/System.map-' + NEW)
                or name.startswith('lib/modules/' + NEW + '/')
                or name.startswith('usr/lib/modules/' + NEW + '/')
                or name.startswith('usr/lib/linux-image-' + NEW + '/')
                or name.startswith('usr/share/doc/linux-image-' + NEW + '/'), 'Unexpected kernel payload path: ' + name)
    require(run(['modinfo', '-F', 'name', pvr]).decode().strip() == 'pvrsrvkm', 'Wrong PVR module name')
    require(run(['modinfo', '-F', 'vermagic', pvr]).decode().startswith(NEW + ' '), 'PVR vermagic mismatch')
    require(b'Rogue_DDK_Linux_WS rogueddk 24.2@6603887' in pvr.read_bytes(), 'PVR DDK mismatch')
    return payload


def object_info(path):
    p = Path(path)
    if not p.exists() and not p.is_symlink():
        return {'type': 'absent'}
    s = p.lstat()
    item = {'mode': stat.S_IMODE(s.st_mode), 'uid': s.st_uid, 'gid': s.st_gid}
    if stat.S_ISLNK(s.st_mode):
        item.update(type='symlink', target=os.readlink(p))
    elif stat.S_ISREG(s.st_mode):
        item.update(type='file', sha256=sha(p), bytes=s.st_size)
    elif stat.S_ISDIR(s.st_mode):
        item.update(type='directory')
    else:
        raise RuntimeError('Special file in transaction: ' + str(p))
    return item


def fixed_baseline_paths():
    paths = list(ALIASES) + [DTB]
    for release in (OLD, PREVIOUS):
        paths += [BOOT / (name + '-' + release) for name in ('vmlinuz', 'initrd.img', 'config', 'System.map')]
        paths += [Path('/usr/lib/modules') / release, Path('/usr/lib/linux-image-' + release),
                  Path('/var/lib/initramfs-tools') / release]
    paths += [Path('/' + name) for name in ('vmlinuz', 'vmlinuz.old', 'initrd.img', 'initrd.img.old')]
    return paths


def baseline_paths():
    # Include every pre-existing boot entry, not just the v9 aliases. Candidate
    # versioned outputs are the only additions excluded from this snapshot.
    outputs = {name + '-' + NEW for name in ('vmlinuz', 'initrd.img', 'uInitrd', 'config', 'System.map')}
    paths = fixed_baseline_paths()
    paths += [p for p in sorted(BOOT.iterdir()) if p.name not in outputs]
    for release in (OLD, PREVIOUS):
        paths += sorted(Path('/var/lib/dpkg/info').glob('linux-image-' + release + '.*'))
    return sorted(set(paths))


def allowed_saved_root(path):
    if not path.is_absolute() or '..' in path.parts:
        return False
    if path in fixed_baseline_paths():
        return True
    outputs = {name + '-' + NEW for name in ('vmlinuz', 'initrd.img', 'uInitrd', 'config', 'System.map')}
    if path.parent == BOOT:
        return path.name not in outputs
    return path.parent == Path('/var/lib/dpkg/info') and any(
        path.name.startswith('linux-image-' + release + '.') for release in (OLD, PREVIOUS))


def snapshot(paths):
    result = {}
    for path in paths:
        item = object_info(path)
        result[str(path)] = item
        if item['type'] == 'directory':
            for child in sorted(path.iterdir()):
                result.update(snapshot([child]))
    return result


def identity_check(expected=RUNNING_RELEASES):
    require(os.geteuid() == 0 and os.uname().machine == 'aarch64', 'Run on the arm64 board as root')
    require(os.uname().release in expected, 'Unexpected running kernel')
    require(Path('/proc/device-tree/model').read_bytes() == b'T736 custom board mainline bring-up\0',
            'Unexpected board model')
    require(run(['findmnt', '-n', '-o', 'SOURCE', '/']).decode().strip() == '/dev/mmcblk0p4', 'Unexpected root device')
    require(run(['blkid', '-s', 'PARTUUID', '-o', 'value', '/dev/mmcblk0p4']).decode().strip().lower() == ROOT_UUID,
            'Unexpected root PARTUUID')
    require('root=PARTUUID=' + ROOT_UUID in Path('/proc/cmdline').read_text().split(), 'Unexpected kernel root argument')


def device_check():
    identity_check()
    require(not run(['dpkg', '--audit']).strip(), 'Configure the existing dpkg transaction before staging')
    for release, version, boot_hashes in ((OLD, '6.18.63-16', BASELINE_BOOT_SHA256),
                                         (PREVIOUS, '6.18.63-27', PREVIOUS_BOOT_SHA256)):
        require(run(['dpkg-query', '-W', '-f=${Status} ${Version}', 'linux-image-' + release]).decode()
                == 'install ok installed ' + version, 'Protected package missing: ' + release)
        for stem, expected_sha in boot_hashes.items():
            path = BOOT / (stem + '-' + release)
            require(path.is_file() and not path.is_symlink() and sha(path) == expected_sha,
                    'Protected boot payload changed: ' + str(path))
        for path in (BOOT / ('initrd.img-' + release), Path('/usr/lib/modules') / release,
                     Path('/usr/lib/linux-image-' + release)):
            require(path.exists() and not path.is_symlink(), 'Incomplete protected release: ' + str(path))
    require(sha(PREVIOUS_DTB) == PREVIOUS_DTB_SHA256, 'Accepted v27 DTB changed')
    previous_pvr = Path('/usr/lib/modules') / PREVIOUS / 'extra/pvrsrvkm.ko'
    require(sha(previous_pvr) == PREVIOUS_PVR_SHA256, 'Accepted v27 PVR changed')
    for alias, stem in ALIASES.items():
        require(alias.is_symlink() and os.readlink(alias) == stem + '-' + OLD, 'Unexpected v16 default alias')
    require(DTB.is_file() and not DTB.is_symlink(), 'Expected regular existing default DTB')
    require(sha(DTB) == BINDINGS['baseline_dtb_sha256'], 'Protected default v16 DTB changed')
    absent = [BOOT / (stem + '-' + NEW) for stem in ('vmlinuz', 'initrd.img', 'uInitrd', 'config', 'System.map')]
    absent += [Path('/usr/lib/modules') / NEW, Path('/usr/lib/linux-image-' + NEW),
               Path('/usr/share/doc/linux-image-' + NEW), Path('/var/lib/initramfs-tools') / NEW]
    require(all(not p.exists() and not p.is_symlink() for p in absent),
            'Candidate output already exists; inspect the prior transaction instead of overwriting it')
    require(not list(Path('/var/lib/dpkg/info').glob('linux-image-' + NEW + '.*')),
            'Candidate package metadata already exists; inspect the prior transaction')


def backup(before, kernel, pvr):
    BACKUPS.mkdir(parents=True, exist_ok=True, mode=0o700)
    info = BACKUPS.lstat()
    require(stat.S_ISDIR(info.st_mode) and info.st_uid == 0 and not (info.st_mode & 0o022), 'Unsafe backup parent')
    directory = Path(tempfile.mkdtemp(prefix=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ-'), dir=BACKUPS))
    blobs = directory / 'blobs'
    blobs.mkdir(mode=0o700)
    for path, item in before.items():
        if item['type'] == 'file':
            target = blobs / item['sha256']
            if not target.exists():
                shutil.copyfile(path, target)
                with target.open('rb') as stream:
                    os.fsync(stream.fileno())
            require(sha(target) == item['sha256'], 'Fallback backup copy mismatch')
    for source, name in ((kernel, 'kernel.deb'), (pvr, 'pvrsrvkm.ko')):
        target = directory / name
        shutil.copyfile(source, target)
        with target.open('rb') as stream:
            os.fsync(stream.fileno())
        require(sha(target) == sha(source), 'Frozen input copy mismatch')
    info_files = sorted(Path('/var/lib/dpkg/info').glob('linux-image-' + OLD + '.*'))
    require(info_files, 'Missing v16 dpkg metadata')
    metadata = directory / 'v16-dpkg-info'
    metadata.mkdir(mode=0o700)
    for source in info_files:
        shutil.copy2(source, metadata / source.name)
        with (metadata / source.name).open('rb') as stream:
            os.fsync(stream.fileno())
    save(directory / 'v16-package-query.txt', run(['dpkg-query', '-s', 'linux-image-' + OLD]))
    record(directory / 'before.json', before)
    record(directory / 'protected-roots.json', [str(p) for p in baseline_paths()])
    record(directory / 'bindings.json', BINDINGS)
    syncdir(blobs)
    syncdir(metadata)
    syncdir(directory)
    syncdir(BACKUPS)
    require(snapshot(baseline_paths()) == before, 'Baseline changed during backup')
    return directory


def replace_object(path, item, directory):
    path = Path(path)
    # A failed hook must not redirect a rollback through a new parent symlink.
    # Directory entries are restored before their children by restore().
    require(all(not parent.is_symlink() for parent in path.parents),
            'Refusing rollback through a symlink parent: ' + str(path))
    if item['type'] == 'directory':
        if path.is_symlink():
            path.unlink()
            syncdir(path.parent)
        require(not path.exists() or path.is_dir(), 'Unexpected nondirectory in rollback: ' + str(path))
        path.mkdir(parents=True, exist_ok=True)
        os.chmod(path, item['mode'])
        os.chown(path, item['uid'], item['gid'])
        syncdir(path)
        return
    if item['type'] == 'absent':
        if path.exists() or path.is_symlink():
            require(not path.is_dir(), 'Refusing to remove unexpected directory')
            path.unlink()
            syncdir(path.parent)
        return
    temporary = path.with_name('.' + path.name + '.t736-v28-' + str(os.getpid()))
    require(not temporary.exists() and not temporary.is_symlink(), 'Stale transaction temporary file')
    try:
        if item['type'] == 'symlink':
            temporary.symlink_to(item['target'])
        else:
            source = directory / 'blobs' / item['sha256']
            require(source.is_file() and sha(source) == item['sha256'], 'Fallback blob corrupted')
            shutil.copyfile(source, temporary)
            os.chmod(temporary, item['mode'])
            with temporary.open('rb') as stream:
                os.fsync(stream.fileno())
        os.chown(temporary, item['uid'], item['gid'], follow_symlinks=False)
        os.replace(temporary, path)
        syncdir(path.parent)
    finally:
        if temporary.exists() or temporary.is_symlink():
            temporary.unlink()


def restore(directory, before):
    # Check all backup bytes before restoring even the first boot-selection file.
    for item in before.values():
        if item['type'] == 'file':
            require(sha(directory / 'blobs' / item['sha256']) == item['sha256'], 'Fallback backup is corrupt')
    # Protected payloads first, aliases last. Candidate files remain for diagnosis;
    # neither protected release is uninstalled or unloaded.
    keys = sorted(before, key=lambda p: (p in {str(x) for x in ALIASES}, p.count('/'), p))
    for path in keys:
        if object_info(path) != before[path]:
            replace_object(path, before[path], directory)
    os.sync()
    require(snapshot(baseline_paths()) == before, 'Rollback verification failed')


def rollback_check(directory):
    identity_check((*RUNNING_RELEASES, NEW))
    require(directory.is_absolute() and not directory.is_symlink()
            and directory.resolve().parent == BACKUPS.resolve(), 'Rollback must use a direct private backup directory')
    info = directory.lstat()
    require(stat.S_ISDIR(info.st_mode) and info.st_uid == 0 and not (info.st_mode & 0o077), 'Unsafe rollback directory')
    before = json.loads((directory / 'before.json').read_text())
    # Use the journaled, bounded roots: a missing boot file must remain
    # restorable, and current directory enumeration is not its authority.
    roots = [Path(p) for p in json.loads((directory / 'protected-roots.json').read_text())]
    require(roots and len(roots) == len(set(roots)) and all(allowed_saved_root(p) for p in roots),
            'Invalid saved protected root list')
    require(set(fixed_baseline_paths()) <= set(roots), 'Missing fixed v16/v27 protection roots')
    root_names = {str(path) for path in roots}
    require(root_names <= before.keys(), 'Incomplete rollback manifest')
    for name, item in before.items():
        path = Path(name)
        require(path.is_absolute() and '..' not in path.parts and
                (name in root_names or any(path.is_relative_to(root) and before[str(root)]['type'] == 'directory'
                                          for root in roots)), 'Rollback path outside fixed baseline')
        require(item.get('type') in ('file', 'symlink', 'directory', 'absent'), 'Invalid rollback object type')
        if item['type'] == 'file':
            require(re.fullmatch('[0-9a-f]{64}', item.get('sha256', '')), 'Invalid rollback blob name')
            source = directory / 'blobs' / item['sha256']
            require(source.is_file() and not source.is_symlink() and source.stat().st_size == item['bytes']
                    and sha(source) == item['sha256'], 'Rollback blob changed')
    for alias, stem in ALIASES.items():
        require(before[str(alias)]['type'] == 'symlink' and before[str(alias)]['target'] == stem + '-' + OLD,
                'Backup does not select pinned v16 release')
    return before


def suppress_hooks(directory):
    require(os.environ.get('T736_V28_CALLER_MNT') not in (None, os.readlink('/proc/self/ns/mnt')),
            'Apply needs a distinct private mount namespace')
    run(['mount', '--make-rprivate', '/'])
    empty = directory / 'empty-hooks'
    empty.mkdir(mode=0o700)
    for path in HOOKS:
        if Path(path).exists():
            require(Path(path).is_dir() and not Path(path).is_symlink(), 'Unexpected hook directory')
            run(['mount', '--bind', empty, path])


def verify_payload(payload):
    for name, expected in payload.items():
        actual = object_info('/' + name)
        # depmod regenerates indexes after the separately supplied PVR is added.
        # Verify these immediately after dpkg; verify immutable payload again
        # after depmod while checking its PVR resolution explicitly below.
        fields = {key: actual.get(key) for key in expected}
        require(fields == expected, 'Installed payload differs: /' + name)


def immutable_payload(payload):
    return {name: item for name, item in payload.items()
            if not (('/modules/' + NEW + '/modules.') in ('/' + name)
                    and Path(name).name not in ('modules.builtin', 'modules.builtin.modinfo', 'modules.order'))}


def final_check(payload, pvr):
    verify_payload(immutable_payload(payload))
    compressed = (BOOT / ('vmlinuz-' + NEW)).read_bytes()
    require(compressed[:2] == b'\x1f\x8b', 'Expected packaged gzip kernel for the existing booti decompression path')
    raw = gzip.decompress(compressed)
    require(len(raw) == BINDINGS['raw_image_bytes'] and hashlib.sha256(raw).hexdigest() == BINDINGS['raw_image_sha256'],
            'Decompressed kernel differs from fixed build Image')
    require(sha(NEW_DTB) == BINDINGS['candidate_dtb_sha256']
            and NEW_DTB.stat().st_size == BINDINGS['candidate_dtb_bytes'],
            'Installed DTB differs from final VE400/readback candidate')
    require(sha(PVR) == BINDINGS['pvr_sha256'], 'Installed PVR bytes changed')
    require(run(['modinfo', '-k', NEW, '-F', 'filename', 'pvrsrvkm']).decode().strip()
            in (str(PVR), str(PVR).replace('/usr/lib/', '/lib/')), 'depmod resolved a different PVR module')
    require(run(['modinfo', '-k', NEW, '-F', 'vermagic', 'pvrsrvkm'])
            == run(['modinfo', '-F', 'vermagic', pvr]), 'Installed PVR vermagic differs')
    require(('CONFIG_CMA_SIZE_MBYTES=256\n') in (BOOT / ('config-' + NEW)).read_text(), 'CMA configuration changed')
    initrd = BOOT / ('initrd.img-' + NEW)
    require(initrd.is_file() and not initrd.is_symlink() and initrd.stat().st_size > 1048576, 'Missing candidate initrd')
    listing = run(['lsinitramfs', initrd]).decode()
    require('lib/modules/' + NEW in listing, 'Initrd lacks the candidate release')
    require(re.search(r'(^|/)(?:sbin|bin)/fsck\.ext4$', listing, re.M), 'Initrd lacks ext4 filesystem repair tool')
    return {'kernel': sha(BOOT / ('vmlinuz-' + NEW)), 'config': sha(BOOT / ('config-' + NEW)),
            'dtb': sha(NEW_DTB), 'pvr': sha(PVR), 'initrd': sha(initrd),
            'modules_dep': sha(Path('/usr/lib/modules') / NEW / 'modules.dep')}


def apply(kernel, pvr, payload, before):
    directory = backup(before, kernel, pvr)
    print('BACKUP_FSYNC_VERIFIED ' + str(directory), flush=True)
    result = {'backup_directory': str(directory), 'running_release': os.uname().release,
              'fallback_release': OLD, 'candidate_release': NEW,
              'rebooted': False, 'hardware_accepted': False, 'candidate_selected': False}
    handlers = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        try:
            suppress_hooks(directory)
            kernel, pvr = directory / 'kernel.deb', directory / 'pvrsrvkm.ko'
            inputs(kernel, pvr)
            run(['dpkg', '--no-triggers', '-i', kernel])
            require(run(['dpkg-query', '-W', '-f=${Status} ${Version}', 'linux-image-' + NEW]).decode()
                    == 'install ok installed ' + VERSION, 'Candidate package not configured')
            verify_payload(payload)
            require(snapshot(baseline_paths()) == before, 'Package changed the v16 baseline or boot selection')
            PVR.parent.mkdir(parents=True, exist_ok=True)
            save(PVR, pvr.read_bytes(), 0o644)
            run(['depmod', '-a', NEW])
            run(['update-initramfs', '-c', '-k', NEW])
            final = final_check(payload, pvr)
            require(snapshot(baseline_paths()) == before, 'Initrd generation changed the v16 baseline or boot selection')
            record(directory / 'verified-candidate.json', final)
            require(snapshot(baseline_paths()) == before, 'Protected v16/v27 state changed during staging')
            result.update(status='STAGED_NOT_SELECTED_NOT_BOOT_TESTED', final_sha256=final,
                          persistent_default_unchanged=True, accepted_v27_unchanged=True,
                          candidate_paths={'kernel': str(BOOT / ('vmlinuz-' + NEW)),
                                           'initrd': str(BOOT / ('initrd.img-' + NEW)),
                                           'dtb': str(NEW_DTB), 'pvr': str(PVR)})
            record(directory / 'result.json', result)
        except BaseException as error:
            result.update(status='UPDATE_FAILED', error=repr(error))
            if isinstance(error, ProcessGroupStillActive):
                result.update(status='CRITICAL_SUBPROCESS_STILL_ACTIVE', rollback_attempted=False)
            else:
                try:
                    restore(directory, before)
                    result['status'] = 'STAGE_FAILED_PROTECTED_FILES_RESTORED'
                except BaseException as rollback:
                    result.update(status='CRITICAL_ROLLBACK_FAILED', rollback_error=repr(rollback))
            try:
                record(directory / 'failure.json', result)
            except BaseException as journal:
                result['journal_error'] = repr(journal)
        print(json.dumps(result, indent=2), flush=True)
        return 0 if result['status'] == 'STAGED_NOT_SELECTED_NOT_BOOT_TESTED' else 2
    finally:
        for sig, handler in handlers.items():
            signal.signal(sig, handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--pvr', type=Path)
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--rollback', type=Path, help='Validate a saved v16/v27 backup; add --apply to restore its protected files')
    args = parser.parse_args()
    if pending():
        print(json.dumps({'status': 'PENDING_FIXED_BUILD_INPUTS', 'missing': pending(), 'write_attempted': False}))
        return 3
    if args.rollback:
        require(not args.kernel and not args.pvr, 'Rollback does not accept candidate inputs')
        before = rollback_check(args.rollback)
        if not args.apply:
            print(json.dumps({'status': 'ROLLBACK_CHECK_PASS', 'backup_directory': str(args.rollback),
                              'write_attempted': False, 'rebooted': False}, indent=2))
            return 0
        with open('/run/lock/t736-media-v28.lock', 'a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            before = rollback_check(args.rollback)
            restore(args.rollback, before)
        print(json.dumps({'status': 'PROTECTED_FILES_RESTORED_NOT_REBOOTED', 'backup_directory': str(args.rollback),
                          'rebooted': False}, indent=2))
        return 0
    require(args.kernel and args.pvr, '--kernel and --pvr are required')
    if args.apply and not os.environ.get('T736_V28_CALLER_MNT'):
        require(os.geteuid() == 0, 'Apply requires root')
        env = dict(os.environ, T736_V28_CALLER_MNT=os.readlink('/proc/self/ns/mnt'))
        os.execvpe('unshare', ['unshare', '--mount', '--propagation', 'private', sys.executable,
                             str(Path(__file__).resolve()), *sys.argv[1:]], env)
    device_check()
    payload = inputs(args.kernel, args.pvr)
    before = snapshot(baseline_paths())
    require(shutil.disk_usage('/var/backups').free > 1024 * 1024 * 1024, 'Need at least 1 GiB free for fallback and installation')
    if not args.apply:
        print(json.dumps({'status': 'CHECK_PASS', 'running_release': os.uname().release, 'fallback_release': OLD, 'candidate_release': NEW,
                          'write_attempted': False, 'hardware_accepted': False,
                          'baseline_files': len(before), 'kernel_sha256': BINDINGS['kernel_sha256'],
                          'pvr_sha256': BINDINGS['pvr_sha256']}, indent=2))
        return 0
    with open('/run/lock/t736-media-v28.lock', 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        device_check()
        require(snapshot(baseline_paths()) == before, 'Baseline changed before lock acquisition')
        return apply(args.kernel, args.pvr, payload, before)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:
        print(json.dumps({'status': 'PREFLIGHT_OR_IO_ERROR', 'error': repr(error), 'rebooted': False}), flush=True)
        sys.exit(1)
