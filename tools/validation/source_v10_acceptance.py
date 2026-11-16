"""Bind the recorded v16 component tests to the finalized v10 inputs.

This verifies preserved evidence; it never marks a newly packed image booted.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess

from .spl_pack_guard import project_file, require, sha
from .cpu_d_pack_guard import PACKAGE_SHA

GPU_RECORD = 'evidence/storage-v16-gpu-acceptance-20261116.json'
GPU_SHA = 'b446a26e7332adc2b9d18d0a5b2e916595a0eb8efc8aa77c39026b50748cc3dd'
KERNEL_SHA = '520b42929ee4700a29ab8da5f020b828948a607c87a6eb8dd98c8ecacd6170c6'
PVR_SHA = 'abd38db687c93b91757719564e1f0fe35b10b137f23d78ea425ff2845d8c8474'
USERSPACE_SHA = '1455889d0c10ec5d0b49e839e0e9d3f7560869e65da5fbee02e477039c78e16e'
RELEASE = '6.18.63-t736-storage-v16'


def check_firefox(record):
    require(record.get('sandbox_disabled') is False and record.get('blocklist_overridden') is False,
            'Firefox test changed sandbox or blocklist')
    baseline = [test for test in record.get('tests', []) if test.get('mode') == 'baseline']
    require(len(baseline) == 1, 'Exactly one Firefox baseline is required')
    baseline = baseline[0]
    require(baseline.get('complete') is True and baseline.get('exit_code') == 0
            and baseline.get('profile_removed') is True, 'Firefox baseline did not complete cleanly')
    require(baseline.get('MOZ_X11_EGL') is None and baseline.get('MOZ_DRM_DEVICE') is None,
            'Firefox baseline requires environment forcing')
    require(not any(name.startswith(('gfx.', 'layers.', 'webgl.')) for name in baseline.get('preferences', {})),
            'Firefox baseline contains graphics overrides')
    capabilities = baseline['session']['capabilities']
    require(capabilities.get('moz:platformVersion') == RELEASE and capabilities.get('moz:headless') is False,
            'Firefox test used a different kernel or headless mode')
    features = baseline['support']['sections']['graphics-features-tbody']
    require(re.search(r'^Compositing\tWebRender$', features, re.M), 'Firefox did not select hardware WebRender')
    renderers = re.findall(r'^WebGL [12] Driver Renderer\t(.+)$', features, re.M)
    require(len(renderers) == 2 and all('PowerVR' in value and not any(
        word in value.lower() for word in ('llvmpipe', 'softpipe', 'software')) for value in renderers),
        'Firefox WebGL hardware renderer missing')
    return {'compositing': 'WebRender', 'webgl_renderers': renderers,
            'browser_version': capabilities['browserVersion'], 'forced_runtime_configuration': False,
            'hardware_video_decoding_accepted': False}


def load_source_v10_acceptance(root, profile, rootfs):
    root = Path(root)
    path = project_file(root, GPU_RECORD)
    require(sha(path) == GPU_SHA, 'Reviewed v16 GPU acceptance record changed')
    gpu = json.loads(path.read_text())
    require(gpu['status'] == 'passed' and gpu['kernel_release'] == RELEASE, 'v16 GPU evidence not passed')
    require(all(gpu['checks'].get(name) is True for name in ('gbm_probe', 'x11_probe',
            'gnome_hardware_renderer', 'gtk_hardware_renderer', 'hdmi_1080p')), 'Incomplete v16 GPU evidence')
    require(profile['kernel_deb_sha256'] == gpu['kernel_deb_sha256'] == KERNEL_SHA
            and profile['module_input_sha256'] == PVR_SHA
            and profile['userspace_deb_sha256'] == gpu['userspace_deb_sha256'] == USERSPACE_SHA,
            'Finalized rootfs input differs from tested components')
    require(profile['desktop_profile'] == 'gpu' and profile['gpu_acceptance_sha256'] == GPU_SHA,
            'Finalized rootfs did not use the reviewed GPU profile')
    # The historical profile manifest omits firmware paths. Require every
    # remaining known boot/GPU member, and independently read all eight actual
    # artifacts (including firmware) from the finalized ext4 before packing.
    for name, expected in gpu['artifacts'].items():
        if not name.startswith('/usr/lib/firmware/'):
            require(profile['installed_files'].get(name) == expected, 'Missing/changed installed artifact: ' + name)
        data = subprocess.run(['debugfs', '-R', 'cat ' + name, str(rootfs)],
                              check=True, capture_output=True).stdout
        require(hashlib.sha256(data).hexdigest() == expected, 'Finalized rootfs artifact differs: ' + name)
    logs = {}
    for name, expected in gpu['evidence_logs'].items():
        path = project_file(root, 'evidence/' + name)
        require(sha(path) == expected, 'Reviewed raw evidence changed: ' + name)
        logs[name] = path
    cold = logs['storage-v16-coldboot-20261116.log'].read_text(errors='replace')
    require('SPL load addr=52000000 bytes=265256 LBA=24693' in cold
            and 'CPU-D: A76 cold416 then warm1014 PASS' in cold
            and 'CPU-D: A55/A76 configured N39/1=1014MHz; DSU and PMIC unchanged' in cold,
            'Cold boot did not confirm the fixed CPU-D initialization')
    cpu = json.loads(logs['storage-v16-cpu-storage-20261116.json'].read_text())
    require(cpu['boot_id'] == gpu['boot_id'] and cpu['kernel'] == RELEASE
            and cpu['status'] == 'PASS' and cpu['online'] == '0-7' and cpu['cpu_seconds'] >= 30,
            'CPU/storage evidence does not describe the accepted v16 boot')
    require({item['cpu'] for item in cpu['cpu']} == set(range(8))
            and all(item['ok'] is True and item['seconds'] >= 30 for item in cpu['cpu']), 'Eight-core load failed')
    registers = logs['storage-v16-cpu-gbm-20261116.log'].read_text(errors='replace')
    expected = {'A55_CTRL': 'f8802700', 'A55_CTRL_STAT': 'f8802700', 'A55_CLK': '03000000',
                'A76_CTRL': 'f8802700', 'A76_CTRL_STAT': 'f8802700', 'A76_CLK': '03000000',
                'DSU_CTRL': 'f8801300', 'DSU_CTRL_STAT': 'f8801300', 'DSU_CLK': '03000105',
                'CPU_SLOW_CLK': '00000000'}
    for name, value in expected.items():
        require(re.search(r't736-cpu-clock-ro: ' + name + r' addr=0x[0-9a-f]+ value=0x' + value, registers),
                'CPU read-only snapshot mismatch: ' + name)
    firefox = check_firefox(json.loads(logs['storage-v16-firefox-renderer-20261116.json'].read_text()))
    return {'scope': 'tested components; new complete image still requires cold boot',
            'cpu_package_sha256': PACKAGE_SHA, 'kernel_release': RELEASE, 'boot_id': gpu['boot_id'],
            'gpu_acceptance_path': GPU_RECORD, 'gpu_acceptance_sha256': GPU_SHA,
            'cpu_d_cold_boot_and_8core_load_verified': True, 'gpu_components_verified': True,
            'firefox': firefox, 'evidence_logs': gpu['evidence_logs'],
            'final_image_cold_boot_validated': False, 'gigabit_ethernet_accepted': False}
