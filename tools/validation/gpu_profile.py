#!/usr/bin/python3
"""Install/restore the persistent T736 desktop profile, live or in an offline root."""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import signal
import subprocess
import sys
import tempfile

RELEASE = '6.18.63-t736-gpu-v15'
PREFIX = '/opt/t736-img-ddk-24.2-x11'
GPU_XORG_PATH = '/usr/share/t736-gpu-profile/20-t736-modesetting-1080p.conf'
TARGETS = {
    'environment': '/etc/environment',
    'xorg': '/etc/X11/xorg.conf.d/20-t736-modesetting.conf',
    'gdm': '/etc/systemd/system/gdm.service.d/90-t736-gpu-profile.conf',
    'modules': '/etc/modules-load.d/t736-gpu.conf',
}
ARTIFACTS = [
    '/boot/vmlinuz-' + RELEASE, '/boot/config-' + RELEASE,
    '/boot/dtb/allwinner/t736-mainline-bringup.dtb',
    '/usr/lib/modules/' + RELEASE + '/extra/pvrsrvkm.ko',
    PREFIX + '/manifest.json',
    '/usr/lib/firmware/rgx.fw.36.56.104.183',
    '/usr/lib/firmware/rgx.sh.36.56.104.183',
    PREFIX + '/runtime-gles.json',
]
CHECKS = ['gbm_probe', 'x11_probe', 'gnome_hardware_renderer', 'gtk_hardware_renderer', 'hdmi_1080p']
BASE_XORG = '''Section "Device"
    Identifier "T736 display engine"
    Driver "modesetting"
    Option "kmsdev" "/dev/dri/card0"
    Option "AccelMethod" "none"
    Option "SWcursor" "true"
    Option "PageFlip" "false"
EndSection
'''
REMOVE_ENV = {'LIBGL_ALWAYS_SOFTWARE', 'GALLIUM_DRIVER', 'MESA_LOADER_DRIVER_OVERRIDE',
              'LD_PRELOAD', '__EGL_VENDOR_LIBRARY_FILENAMES', '__GLX_VENDOR_LIBRARY_NAME',
              'LD_LIBRARY_PATH', 'LIBGL_DRIVERS_PATH', 'COGL_DRIVER'}
REMOVE_ENV |= {'VK_ICD_FILENAMES', 'VK_DRIVER_FILES', 'VK_ADD_DRIVER_FILES'}
REMOVE_ENV |= {'GDK_BACKEND', 'GDK_DISABLE', 'GSK_RENDERER'}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def atomic_write(path, data, mode=0o644, uid=0, gid=0):
    require(not path.is_symlink(), 'Refusing symlink: ' + str(path))
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.' + path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fchmod(stream.fileno(), mode)
            if os.geteuid() == 0:
                os.fchown(stream.fileno(), uid, gid)
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def json_write(path, value, mode=0o600):
    atomic_write(path, (json.dumps(value, indent=2) + '\n').encode(), mode)


class Profile:
    def __init__(self, root):
        self.root = root.resolve(strict=True)
        self.live = self.root == Path('/')
        require((self.root / 'etc').is_dir() and (self.root / 'boot').is_dir(), 'Invalid root directory')
        self.state = self.path('/var/lib/t736-gpu-profile')
        require(not self.state.is_symlink(), 'Invalid profile state directory')
        self.state.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.state.chmod(0o700)
        require(not (self.state / 'lock').is_symlink(), 'Invalid profile lock')
        self.lock = (self.state / 'lock').open('a')
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BaseException:
            self.lock.close()
            raise
        self.baseline = self.state / 'software'
        self.log_path = None

    def path(self, absolute):
        require(absolute.startswith('/') and '..' not in Path(absolute).parts, 'Invalid target path')
        result = self.root / absolute.lstrip('/')
        # usr-merge links are valid for reading artifacts, but all managed config
        # paths and their existing parents must remain inside this root.
        require(result.parent.resolve(strict=False).is_relative_to(self.root), 'Target escapes root: ' + absolute)
        return result

    def command(self, arguments, check=True):
        result = subprocess.run(arguments, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if self.log_path:
            with self.log_path.open('a') as stream:
                stream.write('$ ' + ' '.join(arguments) + '\n' + result.stdout)
        require(not check or result.returncode == 0, 'Command failed: ' + ' '.join(arguments))
        return result

    def guard_console(self):
        for name in ['t736', 'Debian-gdm']:
            uid = pwd.getpwnam(name).pw_uid
            require(uid != 0 and os.environ.get('SUDO_USER') != name, 'Use an independent root console')
            cgroups = Path('/proc/self/cgroup').read_text()
            require(not re.search(r'/user-' + str(uid) + r'\.slice(?:/|$)', cgroups, re.M),
                    'This root shell belongs to the GUI user being terminated')

    def stop_sessions(self):
        errors = []
        if self.command(['systemctl', 'stop', 'gdm.service'], check=False).returncode:
            errors.append('systemctl stop gdm.service')
        commands = []
        for name in ['t736', 'Debian-gdm']:
            uid = pwd.getpwnam(name).pw_uid
            require(uid != 0, 'Refusing to stop UID 0')
            if self.command(['loginctl', 'show-user', str(uid)], check=False).returncode == 0:
                commands.append(['loginctl', 'terminate-user', str(uid)])
            commands.append(['systemctl', 'stop', f'user@{uid}.service', f'user-runtime-dir@{uid}.service'])
        for command in commands:
            if self.command(command, check=False).returncode:
                errors.append(' '.join(command))
        for name in ['t736', 'Debian-gdm']:
            uid = pwd.getpwnam(name).pw_uid
            result = self.command(['systemctl', 'show', f'user@{uid}.service', '-p', 'ActiveState', '--value'], check=False)
            if result.returncode or result.stdout.strip() not in ['inactive', 'failed']:
                errors.append(f'user@{uid}.service still active')
        require(not errors, 'Unable to stop GUI sessions: ' + '; '.join(errors))

    def start_gdm(self):
        self.command(['systemctl', 'daemon-reload'])
        self.command(['systemctl', 'reset-failed', 'gdm.service'], check=False)
        self.command(['systemctl', 'start', 'gdm.service'])
        self.command(['systemctl', 'is-active', '--quiet', 'gdm.service'])

    def baseline_ready(self):
        manifest_path = self.baseline / 'manifest.json'
        if manifest_path.exists():
            saved = json.loads(manifest_path.read_text())
            require(set(saved) == set(TARGETS), 'Invalid software baseline manifest')
            for name, record in saved.items():
                if record['present']:
                    require(digest(self.baseline / name) == record['sha256'], 'Software baseline changed: ' + name)
            return saved
        require(not self.baseline.exists(), 'Incomplete software baseline; inspect state first')
        require(self.path(TARGETS['xorg']).read_text() == BASE_XORG, 'Capture baseline only from the known software Xorg config')
        text = self.path(TARGETS['environment']).read_text()
        require(re.search(r'^\s*LIBGL_ALWAYS_SOFTWARE\s*=\s*(?:true|1)\s*$', text, re.M)
                and re.search(r'^\s*GALLIUM_DRIVER\s*=\s*llvmpipe\s*$', text, re.M),
                'Capture baseline only from the software environment')
        pending = self.state / 'software.new'
        require(not pending.exists(), 'Incomplete baseline staging; inspect state first')
        pending.mkdir(mode=0o700)
        saved = {}
        for name, absolute in TARGETS.items():
            path = self.path(absolute)
            require(not path.is_symlink(), 'Refusing managed symlink: ' + absolute)
            record = {'present': path.exists()}
            if path.exists():
                require(path.is_file(), 'Not a regular configuration file: ' + absolute)
                stat = path.stat()
                record.update(mode=stat.st_mode & 0o7777, uid=stat.st_uid, gid=stat.st_gid, sha256=digest(path))
                atomic_write(pending / name, path.read_bytes(), 0o600)
            saved[name] = record
        json_write(pending / 'manifest.json', saved)
        pending.rename(self.baseline)
        return saved

    def restore(self, saved):
        errors = []
        for name, absolute in TARGETS.items():
            try:
                path, record = self.path(absolute), saved[name]
                require(not path.is_symlink(), 'Refusing managed symlink: ' + absolute)
                if record['present']:
                    atomic_write(path, (self.baseline / name).read_bytes(), record['mode'], record['uid'], record['gid'])
                else:
                    path.unlink(missing_ok=True)
            except Exception as error:
                errors.append(str(error))
        require(not errors, 'Software restore failed: ' + '; '.join(errors))

    def verify_evidence(self, evidence_path):
        require(evidence_path is not None, 'GPU profile requires --evidence from successful board tests')
        evidence = json.loads(evidence_path.read_text())
        require(evidence.get('schema') == 1 and evidence.get('board') == 't736'
                and evidence.get('kernel_release') == RELEASE and evidence.get('status') == 'passed',
                'Unsupported or failed GPU acceptance record')
        require(re.fullmatch(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', evidence.get('boot_id', '')),
                'Acceptance record requires the tested boot ID')
        require(all(evidence.get('checks', {}).get(name) is True for name in CHECKS), 'Incomplete GPU board acceptance')
        renderer = evidence.get('renderer', '')
        require(any(name in renderer for name in ['BXM', 'PowerVR', 'Imagination'])
                and not any(name in renderer.lower() for name in ['llvmpipe', 'softpipe', 'software']),
                'Acceptance record is not an IMG hardware renderer')
        require(re.fullmatch(r'[0-9a-f]{64}', evidence.get('kernel_deb_sha256', '')), 'Missing kernel package hash')
        require(re.fullmatch(r'[0-9a-f]{64}', evidence.get('userspace_deb_sha256', '')), 'Missing userspace package hash')
        require(evidence.get('evidence_logs') and all(re.fullmatch(r'[0-9a-f]{64}', value)
                for value in evidence['evidence_logs'].values()), 'Missing evidence log hashes')
        hashes = evidence.get('artifacts', {})
        require(set(hashes) == set(ARTIFACTS), 'Acceptance record must cover the exact required boot/GPU artifacts')
        for absolute in ARTIFACTS:
            require(digest(self.path(absolute)) == hashes[absolute], 'Acceptance artifact mismatch: ' + absolute)
        manifest = json.loads(self.path(PREFIX + '/manifest.json').read_text())
        require(manifest.get('version') == '24.2.6603887-2' and manifest.get('prefix') == PREFIX, 'GPU profile requires IMG runtime-gles package -2')
        for record in manifest['files']:
            path = self.path('/' + record['path'])
            if 'symlink' in record:
                require(path.is_symlink() and os.readlink(path) == record['symlink'], 'GPU symlink mismatch: ' + str(path))
            elif 'sha256' in record:
                require(digest(path) == record['sha256'], 'GPU payload mismatch: ' + str(path))
        for name, expected in manifest['firmware_sha256'].items():
            require(digest(self.path('/usr/lib/firmware/' + name)) == expected, 'GPU firmware mismatch: ' + name)
        helper = PREFIX + '/bin/gpu-runtime-view'
        require(any(record.get('path') == helper.lstrip('/') and 'sha256' in record
                    for record in manifest['files']), 'Runtime verifier is not bound to the payload manifest')
        # This verified Python helper checks files only, including in --root mode.
        result = subprocess.run([sys.executable, str(self.path(helper)), '--root', str(self.root), '--verify'],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        require(result.returncode == 0, 'Runtime view verification failed: ' + result.stdout)
        if self.live:
            require(os.uname().release == RELEASE, 'GPU enable requires the validated running kernel')
            require(Path('/sys/module/pvrsrvkm').is_dir(), 'Load/validate pvrsrvkm first')
            nodes = [p for p in Path('/sys/class/drm').glob('renderD*')
                     if (p / 'device/driver/module').resolve().name == 'pvrsrvkm']
            require(len(nodes) == 1, 'Expected one pvrsrvkm render node')
        return evidence

    def apply_gpu(self, saved):
        lines = []
        for line in (self.baseline / 'environment').read_text().splitlines(keepends=True):
            match = re.match(r'^\s*(?:export\s+)?([A-Za-z_][A-Za-z_0-9]*)\s*=', line)
            if not match or match.group(1) not in REMOVE_ENV:
                lines.append(line)
        text = ''.join(lines)
        text += '' if not text or text.endswith('\n') else '\n'
        text += f'LD_LIBRARY_PATH={PREFIX}/runtime-gles\nLIBGL_DRIVERS_PATH={PREFIX}/runtime-gles/dri\nCOGL_DRIVER=gles2\n'
        text += 'GDK_BACKEND=x11\nGDK_DISABLE=gl-api,glx,vulkan\nGSK_RENDERER=opengl\n'
        env = saved['environment']
        atomic_write(self.path(TARGETS['environment']), text.encode(), env['mode'], env['uid'], env['gid'])
        xorg = saved['xorg']
        atomic_write(self.path(TARGETS['xorg']), self.path(GPU_XORG_PATH).read_bytes(),
                     xorg['mode'], xorg['uid'], xorg['gid'])
        dropin = ('[Unit]\nAfter=systemd-modules-load.service\n[Service]\nEnvironmentFile=/etc/environment\n'
                  'UnsetEnvironment=LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE '
                  'LD_PRELOAD __EGL_VENDOR_LIBRARY_FILENAMES __GLX_VENDOR_LIBRARY_NAME '
                  'VK_ICD_FILENAMES VK_DRIVER_FILES VK_ADD_DRIVER_FILES\n')
        atomic_write(self.path(TARGETS['gdm']), dropin.encode())
        atomic_write(self.path(TARGETS['modules']), b'pvrsrvkm\n')

    def apply(self, selected, evidence_path):
        require(not (self.state / 'pending.json').exists() or selected == 'software',
                'An interrupted transaction exists; recover with software first')
        evidence = self.verify_evidence(evidence_path) if selected == 'gpu' else None
        if self.live:
            self.guard_console()
            require(not Path('/root/t736-gpu-session-trial/active').exists(), 'Roll back the temporary session trial before switching persistent profiles')
        saved = self.baseline_ready()
        transaction = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + str(os.getpid())
        self.log_path = self.state / (transaction + '.log')
        json_write(self.state / 'pending.json', {'requested': selected, 'transaction': transaction})
        try:
            if self.live:
                self.stop_sessions()
            if selected == 'gpu':
                self.apply_gpu(saved)
            else:
                self.restore(saved)
            if self.live:
                self.start_gdm()
            if evidence:
                json_write(self.state / 'accepted-gpu.json', evidence)
            json_write(self.state / 'profile.json', {'profile': selected, 'transaction': transaction,
                       'acceptance_sha256': digest(self.state / 'accepted-gpu.json') if evidence else None,
                       'final_image_cold_boot_validated': False})
            (self.state / 'pending.json').unlink()
        except BaseException:
            # Restore all baseline files even if stopping a session failed.
            try:
                stop_error = None
                if self.live:
                    try:
                        self.stop_sessions()
                    except Exception as error:
                        stop_error = error
                self.restore(saved)
                if self.live:
                    self.start_gdm()
                if stop_error:
                    raise stop_error
                json_write(self.state / 'profile.json', {'profile': 'software', 'transaction': transaction,
                           'recovered_after_failure': True, 'final_image_cold_boot_validated': False})
                (self.state / 'pending.json').unlink(missing_ok=True)
            except Exception as recovery_error:
                print('Software recovery incomplete; rerun software from root console: ' + str(recovery_error), file=sys.stderr)
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile', choices=['status', 'software', 'gpu'])
    parser.add_argument('--root', type=Path, default=Path('/'), help='Offline mounted root; default is live system')
    parser.add_argument('--evidence', type=Path, help='Passed GPU board acceptance JSON bound to exact installed artifacts')
    args = parser.parse_args()
    require(os.geteuid() == 0, 'Run as root')
    profile = Profile(args.root)
    if args.profile == 'status':
        state = profile.state / 'profile.json'
        print(json.dumps({'root': str(profile.root), 'state': json.loads(state.read_text()) if state.exists() else None,
                          'interrupted_transaction': (profile.state / 'pending.json').exists()}, indent=2))
    else:
        profile.apply(args.profile, args.evidence)
        print('Configured ' + args.profile + '; final image cold-boot/display acceptance remains separate.')


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise InterruptedError('Signal ' + str(signum))
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        print('ERROR: ' + str(error), file=sys.stderr)
        sys.exit(1)
