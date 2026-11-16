"""Offline fixtures only: no board, services, mounts or real acceptance claims."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('gpu_profile', HERE / 'gpu_profile.py')
gpu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpu)


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())


class ProfileFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='t736-gpu-profile-fixture-')
        self.root = Path(self.temp.name)
        self.original = {
            'environment': '# preserve\nLANG=zh_CN.UTF-8\nCUSTOM="two words"\nLIBGL_ALWAYS_SOFTWARE=true\nGALLIUM_DRIVER=llvmpipe\n',
            'xorg': gpu.BASE_XORG,
            'gdm': '[Service]\n# preserve prior owned drop-in\n',
            'modules': 'existing-module\n',
        }
        for name, text in self.original.items():
            write(self.root / gpu.TARGETS[name].lstrip('/'), text)
        write(self.root / gpu.GPU_XORG_PATH.lstrip('/'),
              (HERE.parents[1] / 'config/gpu-v10/20-t736-modesetting-1080p.conf').read_bytes())
        (self.root / 'etc/environment').chmod(0o640)
        for absolute in gpu.ARTIFACTS:
            if not absolute.endswith(('/manifest.json', '/runtime-gles.json')):
                write(self.root / absolute.lstrip('/'), 'SYNTHETIC OFFLINE FIXTURE ' + absolute)
        private = self.root / gpu.PREFIX.lstrip('/')
        write(private / 'lib/libEGL.so.1', 'synthetic EGL payload')
        for name in ['libGLESv2.so.2', 'libgbm.so.1', 'libglapi.so.0', 'libvulkan.so.1']:
            write(private / 'lib' / name, 'synthetic ' + name)
        (private / 'lib/dri').mkdir()
        (private / 'lib/libEGL.so').symlink_to('libEGL.so.1')
        write(private / 'bin/t736-img-run', 'synthetic wrapper')
        write(private / 'bin/gpu-runtime-view', (HERE / 'gpu_runtime_view.py').read_bytes())
        (self.root / 'usr/bin').mkdir(parents=True)
        (self.root / 'usr/bin/t736-img-run').symlink_to(gpu.PREFIX + '/bin/t736-img-run')
        manifest = {'version': '24.2.6603887-2', 'prefix': gpu.PREFIX,
                    'firmware_sha256': {Path(p).name: gpu.digest(self.root / p.lstrip('/'))
                                       for p in gpu.ARTIFACTS if '/firmware/' in p},
                    'files': [
                        {'path': gpu.PREFIX.lstrip('/') + '/lib/libEGL.so.1', 'sha256': gpu.digest(private / 'lib/libEGL.so.1')},
                        {'path': gpu.PREFIX.lstrip('/') + '/lib/libEGL.so', 'symlink': 'libEGL.so.1'},
                        {'path': gpu.PREFIX.lstrip('/') + '/bin/gpu-runtime-view', 'sha256': gpu.digest(private / 'bin/gpu-runtime-view')},
                        {'path': 'usr/bin/t736-img-run', 'symlink': gpu.PREFIX + '/bin/t736-img-run'}]}
        write(private / 'manifest.json', json.dumps(manifest))
        subprocess.run([sys.executable, str(private / 'bin/gpu-runtime-view'), '--root', str(self.root)],
                       check=True, capture_output=True)
        self.evidence = self.root / 'synthetic-acceptance.json'
        self.record = {'schema': 1, 'board': 't736', 'kernel_release': gpu.RELEASE, 'status': 'passed',
                       'boot_id': '00000000-0000-0000-0000-000000000001', 'renderer': 'BXM SYNTHETIC FIXTURE',
                       'checks': {key: True for key in gpu.CHECKS}, 'kernel_deb_sha256': 'c' * 64,
                       'userspace_deb_sha256': 'e' * 64,
                       'evidence_logs': {'synthetic-fixture-only.log': 'd' * 64},
                       'artifacts': {p: gpu.digest(self.root / p.lstrip('/')) for p in gpu.ARTIFACTS}}
        write(self.evidence, json.dumps(self.record))
        self.profile = gpu.Profile(self.root)
        self.profile.command = lambda *a, **k: self.fail('Offline mode invoked a service command')

    def tearDown(self):
        self.profile.lock.close()
        self.temp.cleanup()

    def assert_software(self):
        for name, text in self.original.items():
            self.assertEqual((self.root / gpu.TARGETS[name].lstrip('/')).read_text(), text)
        self.assertEqual((self.root / 'etc/environment').stat().st_mode & 0o777, 0o640)
        self.assertFalse((self.profile.state / 'pending.json').exists())

    def test_install_gpu_and_restore_exact_baseline(self):
        self.profile.apply('software', None)
        self.assert_software()
        self.profile.apply('gpu', self.evidence)
        env = (self.root / 'etc/environment').read_text()
        self.assertIn('CUSTOM="two words"', env)
        self.assertIn('COGL_DRIVER=gles2', env)
        self.assertIn('GDK_BACKEND=x11\n', env)
        self.assertIn('GDK_DISABLE=gl-api,glx,vulkan\n', env)
        self.assertIn('GSK_RENDERER=opengl\n', env)
        self.assertIn('LD_LIBRARY_PATH=' + gpu.PREFIX + '/runtime-gles\n', env)
        self.assertNotIn('LIBGL_ALWAYS_SOFTWARE', env)
        xorg = (self.root / gpu.TARGETS['xorg'].lstrip('/')).read_text()
        self.assertIn('"AccelMethod" "glamor"', xorg)
        self.assertIn('"PageFlip" "false"', xorg)
        self.assertIn('Option "PreferredMode" "1920x1080"', xorg)
        self.assertIn('DefaultDepth 24', xorg)
        self.assertIn('Modes "1920x1080"', xorg)
        self.assertIn('Virtual 1920 1080', xorg)
        self.assertEqual((self.root / gpu.TARGETS['modules'].lstrip('/')).read_text(), 'pvrsrvkm\n')
        self.profile.apply('software', None)
        self.assert_software()

    def test_reject_missing_or_failed_acceptance_before_changes(self):
        with self.assertRaisesRegex(RuntimeError, 'requires --evidence'):
            self.profile.apply('gpu', None)
        self.record['checks']['gnome_hardware_renderer'] = False
        write(self.evidence, json.dumps(self.record))
        with self.assertRaisesRegex(RuntimeError, 'Incomplete GPU'):
            self.profile.apply('gpu', self.evidence)
        self.assert_software()

    def test_reject_different_module_bytes_under_same_release(self):
        write(self.root / gpu.ARTIFACTS[3].lstrip('/'), 'a different module build')
        with self.assertRaisesRegex(RuntimeError, 'artifact mismatch'):
            self.profile.apply('gpu', self.evidence)
        self.assert_software()

    def test_reject_gtk_software_fallback_before_changes(self):
        self.record['checks']['gtk_hardware_renderer'] = False
        write(self.evidence, json.dumps(self.record))
        with self.assertRaisesRegex(RuntimeError, 'Incomplete GPU'):
            self.profile.apply('gpu', self.evidence)
        self.assert_software()

    def test_reject_payload_symlink_change(self):
        link = self.root / gpu.PREFIX.lstrip('/') / 'lib/libEGL.so'
        link.unlink()
        link.symlink_to('wrong.so')
        with self.assertRaisesRegex(RuntimeError, 'symlink mismatch'):
            self.profile.apply('gpu', self.evidence)
        self.assert_software()

    def test_reject_vendor_vulkan_in_runtime_view(self):
        private = self.root / gpu.PREFIX.lstrip('/')
        (private / 'runtime-gles/libvulkan.so.1').symlink_to('../lib/libvulkan.so.1')
        with self.assertRaisesRegex(RuntimeError, 'Runtime view verification failed'):
            self.profile.apply('gpu', self.evidence)
        self.assert_software()

    def test_midwrite_failure_restores_software(self):
        real_write = gpu.atomic_write
        fired = []
        def injected(path, data, *args, **kwargs):
            if path == self.root / gpu.TARGETS['xorg'].lstrip('/') and b'glamor' in data and not fired:
                fired.append(True)
                raise OSError('injected Xorg write failure')
            return real_write(path, data, *args, **kwargs)
        with patch.object(gpu, 'atomic_write', injected):
            with self.assertRaisesRegex(OSError, 'injected'):
                self.profile.apply('gpu', self.evidence)
        self.assertTrue(fired)
        self.assert_software()
        self.assertTrue(json.loads((self.profile.state / 'profile.json').read_text())['recovered_after_failure'])

    def test_interrupted_transaction_can_recover_without_evidence(self):
        self.profile.apply('gpu', self.evidence)
        write(self.profile.state / 'pending.json', '{"requested":"gpu"}')
        with self.assertRaisesRegex(RuntimeError, 'interrupted transaction'):
            self.profile.apply('gpu', self.evidence)
        self.profile.apply('software', None)
        self.assert_software()

    def test_second_writer_is_locked_out(self):
        with self.assertRaises(BlockingIOError):
            gpu.Profile(self.root)


class PackManifestFixture(unittest.TestCase):
    def test_rootfs_profile_metadata_is_bound_to_external_manifest(self):
        self.check_manifest_binding('v9')

    def test_v10_metadata_uses_its_own_build_state(self):
        self.check_manifest_binding('v10')

    def check_manifest_binding(self, variant):
        if not shutil.which('mke2fs') or not shutil.which('debugfs'):
            self.skipTest('e2fsprogs needed for tiny offline image fixture')
        sys.path.insert(0, str(HERE.parent))
        import pack_t736_source_boot as pack
        with tempfile.TemporaryDirectory(prefix='t736-v9-pack-fixture-') as temporary:
            root = Path(temporary)
            staging = root / 'staging'
            record = {'variant': variant, 'desktop_profile': 'software',
                      'kernel_release': {'v9': '6.18.63-t736-gpu-v15', 'v10': '6.18.63-t736-storage-v16'}[variant]}
            saved = root / f'build/source-rootfs-{variant}-state/profile-manifest.json'
            write(saved, json.dumps(record))
            write(staging / 'usr/share/doc/t736-gpu-profile/build-manifest.json', json.dumps(record))
            image = root / 'tiny-fixture.ext4'
            with image.open('wb') as stream:
                stream.truncate(16 * 1024 * 1024)
            subprocess.run(['mke2fs', '-q', '-t', 'ext4', '-F', '-d', str(staging), str(image)], check=True, capture_output=True)
            self.assertEqual(pack.read_source_profile_record(root, image, variant), record)
            record['desktop_profile'] = 'gpu'
            write(saved, json.dumps(record))
            with self.assertRaisesRegex(AssertionError, 'differs'):
                pack.read_source_profile_record(root, image, variant)


if __name__ == '__main__':
    unittest.main()
