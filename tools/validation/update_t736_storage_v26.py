#!/usr/bin/env python3
"""T736 v26 transaction; running v16/v25, fixed v16 fallback; check is default.

This candidate uses standard noncoherent DMA payload allocation and explicit
ownership synchronization; descriptors remain coherent. Final build bindings are fixed below. This is not hardware acceptance. It never reboots or writes bootcmd,
SPL, GPT or raw media. Apply uses a private mount namespace to suppress kernel
package/initramfs hooks until verified files are selected by the existing v9
boot aliases. Failed transactions restore the saved v16 boot selection.
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
NEW = '6.18.63-t736-storage-v26'
VERSION = '6.18.63-26'
PREVIOUS = '6.18.63-t736-storage-v25'
RUNNING_RELEASES = (OLD, PREVIOUS)
PREVIOUS_DTB = Path('/usr/lib/linux-image-' + PREVIOUS + '/allwinner/t736-gpu-candidate.dtb')
PREVIOUS_DTB_SHA256 = '07824de9f58de27e0aebfaf70e4da464d145570e0235f17bcbd01abed39ff244'
ROOT_UUID = 'a0085546-4166-744a-a353-fca9272b8e48'
BOOT = Path('/boot')
ALIASES = {BOOT / (stem + '-6.18.63-t736-armbian'): stem for stem in ('vmlinuz', 'initrd.img')}
DTB = BOOT / 'dtb/allwinner/t736-mainline-bringup.dtb'
NEW_DTB = Path('/usr/lib/linux-image-' + NEW + '/allwinner/t736-gpu-candidate.dtb')
PVR = Path('/usr/lib/modules') / NEW / 'extra/pvrsrvkm.ko'
BACKUPS = Path('/var/backups/t736-storage-v26')
# Fixed package/PVR inputs; gzip kernel is preserved for the existing booti
# decompression path. These hashes are build provenance, not board acceptance.
# Final independently verified v26 build bindings; board validation is separate.
# Opt-in timing collection does not change the fixed v16 capacity-topology fallback.
BINDINGS = {'baseline_dtb_sha256': '6124591aab1cfc02b287a88872835b20a6d2dac70b3e7cd2b93bb9af36d3e82b', 'candidate_dtb_bytes': 12028, 'candidate_dtb_sha256': '07824de9f58de27e0aebfaf70e4da464d145570e0235f17bcbd01abed39ff244', 'control_manifest_sha256': '1ecf2b92707bc7ea859258b82a86fe1ae69ab706c78b01432896b638f42e964a', 'kernel_bytes': 5604794, 'kernel_sha256': 'f270f50a59da295f2104512a8504531698eab2967d769e17c8ba322a2b075281', 'payload_manifest_sha256': 'bf32b05ca819fba0f8016700e0823e037f7dd206b8c9a973c6f3e200e80ab920', 'pvr_bytes': 1913816, 'pvr_sha256': '57e217ef2aa31c02b6f2a41e7a21d6361e2532f628ed34966ac1febc1dac255b', 'raw_image_bytes': 11800584, 'raw_image_sha256': '59010d0b1e6e41a769c4c1bc0063d63af2a42944e4ffcea1f520d32e13c6d8a1'}
BASELINE_BOOT_SHA256 = {'vmlinuz': '0244c994eb6092e2d68f8ae4bef9347c9ce11507790b8eb351db1a5a8023ccee', 'config': 'c2a5adf5941401efa3a4b474fb43afae3183049ec6a5c61194c4f2aeedb6449e', 'System.map': 'ebdda5fb6f14eacb6775602f3cc245af86c87f4780d6d4a046c886d29d608ea5'}
PREVIOUS_BOOT_SHA256 = {'vmlinuz': 'debd6b6edbd88db3291c3353267e041df527d424f4f6794cdf90c338b9e596db', 'config': 'e00c797cf8e3eead58d5954e81bf9f95a087ba0acf92d4191cc4dbec3418b960', 'System.map': '82f0da9f46797b9dae27f8f928e689f49e75b29b12f186d24854a896ed4b8b57'}
HOOKS = ('/etc/kernel', '/usr/share/kernel', '/etc/initramfs/post-update.d')


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
        try:
            process.communicate(timeout=10)
        except subprocess.TimeoutExpired as stop_error:
            raise ProcessGroupStillActive('Killed subprocess did not close its pipes') from stop_error
        wait_group_stopped(process.pid)
        raise
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
            'Package DTB differs from the fixed final DMA/topology candidate')
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


def baseline_paths():
    paths = list(ALIASES) + [DTB]
    paths += [BOOT / (name + '-' + OLD) for name in ('vmlinuz', 'initrd.img', 'config', 'System.map')]
    paths += [Path('/usr/lib/modules') / OLD, Path('/usr/lib/linux-image-' + OLD)]
    # Generic Debian links are preserved too, though the fixed v9 bootcmd uses
    # only the two aliases above. Package hooks are not allowed to change them.
    paths += [Path('/' + name) for name in ('vmlinuz', 'vmlinuz.old', 'initrd.img', 'initrd.img.old')]
    return paths


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
    if os.uname().release == PREVIOUS:
        require(run(['dpkg-query', '-W', '-f=${Status} ${Version}', 'linux-image-' + PREVIOUS]).decode()
                == 'install ok installed 6.18.63-25', 'Validated running v25 package missing')
        for stem, expected_sha in PREVIOUS_BOOT_SHA256.items():
            require(sha(BOOT / (stem + '-' + PREVIOUS)) == expected_sha,
                    'Validated running v25 boot payload changed: ' + stem)
        require(sha(PREVIOUS_DTB) == PREVIOUS_DTB_SHA256, 'Validated v25 package DTB changed')
    require(Path('/proc/device-tree/model').read_bytes() == b'T736 custom board mainline bring-up\0',
            'Unexpected board model')
    require(run(['findmnt', '-n', '-o', 'SOURCE', '/']).decode().strip() == '/dev/mmcblk0p4', 'Unexpected root device')
    require(run(['blkid', '-s', 'PARTUUID', '-o', 'value', '/dev/mmcblk0p4']).decode().strip().lower() == ROOT_UUID,
            'Unexpected root PARTUUID')
    require('root=PARTUUID=' + ROOT_UUID in Path('/proc/cmdline').read_text().split(), 'Unexpected kernel root argument')


def device_check():
    identity_check()
    require(not run(['dpkg', '--audit']).strip(), 'Configure the existing dpkg transaction before this update')
    require(run(['dpkg-query', '-W', '-f=${Status} ${Version}', 'linux-image-' + OLD]).decode()
            == 'install ok installed 6.18.63-16', 'Baseline v16 package missing')
    for alias, stem in ALIASES.items():
        require(alias.is_symlink() and os.readlink(alias) == stem + '-' + OLD, 'Unexpected v9 boot alias')
    require(DTB.is_file() and not DTB.is_symlink(), 'Expected regular existing boot DTB')
    require(sha(DTB) == BINDINGS['baseline_dtb_sha256'],
            'Baseline DTB differs from the explicitly accepted v16 topology state')
    for stem, expected_sha in BASELINE_BOOT_SHA256.items():
        require(sha(BOOT / (stem + '-' + OLD)) == expected_sha,
                'Baseline v16 boot payload changed: ' + stem)
    for path in baseline_paths()[3:9]:
        require(path.exists() and not path.is_symlink(), 'Incomplete/nonregular v16 fallback: ' + str(path))
    require(not (Path('/usr/lib/modules') / NEW).exists() and not (BOOT / ('vmlinuz-' + NEW)).exists(),
            'Candidate already installed; inspect prior transaction rather than overwriting it')


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
    record(directory / 'bindings.json', BINDINGS)
    syncdir(blobs)
    syncdir(metadata)
    syncdir(directory)
    syncdir(BACKUPS)
    require(snapshot(baseline_paths()) == before, 'Baseline changed during backup')
    return directory


def replace_object(path, item, directory):
    path = Path(path)
    if item['type'] == 'directory':
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
    temporary = path.with_name('.' + path.name + '.t736-v26-' + str(os.getpid()))
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
    # Payloads first, aliases last. v26 remains installed for diagnosis; v16 is
    # never uninstalled and neither running kernel is unloaded.
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
    roots = baseline_paths()
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
    require(os.environ.get('T736_V26_CALLER_MNT') not in (None, os.readlink('/proc/self/ns/mnt')),
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
            'Installed DTB differs from final DMA/topology candidate')
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


def select_candidate(directory, final):
    blob = directory / 'blobs' / final['dtb']
    if not blob.exists():
        save(blob, NEW_DTB.read_bytes())
    replace_object(DTB, {'type': 'file', 'mode': 0o644, 'uid': 0, 'gid': 0, 'sha256': final['dtb']}, directory)
    for alias, stem in ALIASES.items():
        replace_object(alias, {'type': 'symlink', 'target': stem + '-' + NEW, 'uid': 0, 'gid': 0}, directory)
    os.sync()
    require(sha(DTB) == final['dtb'], 'Selected DTB readback differs')
    for alias, stem in ALIASES.items():
        require(alias.is_symlink() and os.readlink(alias) == stem + '-' + NEW, 'Selected alias differs')
        require(sha(alias) == final['kernel' if stem == 'vmlinuz' else 'initrd'], 'Selected alias bytes differ')


def apply(kernel, pvr, payload, before):
    directory = backup(before, kernel, pvr)
    print('BACKUP_FSYNC_VERIFIED ' + str(directory), flush=True)
    result = {'backup_directory': str(directory), 'running_release': os.uname().release,
              'fallback_release': OLD, 'candidate_release': NEW,
              'rebooted': False, 'hardware_accepted': False}
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
            select_candidate(directory, final)
            unchanged = {path: item for path, item in before.items() if path not in {str(DTB), *(str(x) for x in ALIASES)}}
            require(all(object_info(path) == item for path, item in unchanged.items()), 'v16 fallback changed during selection')
            result.update(status='INSTALLED_AND_SELECTED_NOT_BOOT_TESTED', final_sha256=final)
            record(directory / 'result.json', result)
        except BaseException as error:
            result.update(status='UPDATE_FAILED', error=repr(error))
            if isinstance(error, ProcessGroupStillActive):
                result.update(status='CRITICAL_SUBPROCESS_STILL_ACTIVE', rollback_attempted=False)
            else:
                try:
                    restore(directory, before)
                    result['status'] = 'UPDATE_FAILED_V16_RESTORED'
                except BaseException as rollback:
                    result.update(status='CRITICAL_ROLLBACK_FAILED', rollback_error=repr(rollback))
            try:
                record(directory / 'failure.json', result)
            except BaseException as journal:
                result['journal_error'] = repr(journal)
        print(json.dumps(result, indent=2), flush=True)
        return 0 if result['status'] == 'INSTALLED_AND_SELECTED_NOT_BOOT_TESTED' else 2
    finally:
        for sig, handler in handlers.items():
            signal.signal(sig, handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--pvr', type=Path)
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--rollback', type=Path, help='Validate a saved v16 backup; add --apply to restore its files/aliases')
    args = parser.parse_args()
    if args.rollback:
        require(not args.kernel and not args.pvr, 'Rollback does not accept candidate inputs')
        before = rollback_check(args.rollback)
        if not args.apply:
            print(json.dumps({'status': 'ROLLBACK_CHECK_PASS', 'backup_directory': str(args.rollback),
                              'write_attempted': False, 'rebooted': False}, indent=2))
            return 0
        with open('/run/lock/t736-storage-v26.lock', 'a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            before = rollback_check(args.rollback)
            restore(args.rollback, before)
        print(json.dumps({'status': 'V16_RESTORED_NOT_REBOOTED', 'backup_directory': str(args.rollback),
                          'rebooted': False}, indent=2))
        return 0
    if pending():
        print(json.dumps({'status': 'PENDING_FIXED_BUILD_INPUTS', 'missing': pending(), 'write_attempted': False}))
        return 3
    require(args.kernel and args.pvr, '--kernel and --pvr are required')
    if args.apply and not os.environ.get('T736_V26_CALLER_MNT'):
        require(os.geteuid() == 0, 'Apply requires root')
        env = dict(os.environ, T736_V26_CALLER_MNT=os.readlink('/proc/self/ns/mnt'))
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
    with open('/run/lock/t736-storage-v26.lock', 'a') as lock:
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
