#!/usr/bin/env python3
"""Add the reviewed DCDC2 readback to an already built isolated v28 tree.

This is a one-time incremental build, never a deployment. The first v28
artifacts and record are preserved before applying the additional source.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('/home/ubuntu/t736-mainline'))
    parser.add_argument('--jobs', type=int, default=12)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root/'sources/work/linux-media-v28'
    baseline = root/'sources/work/linux-media-v27'
    out = root/'build/linux-media-v28'
    baseline_out = root/'build/linux-media-v27'
    pvr_source = root/'sources/work/pvr-bxm-media-v28-24.2'
    pvr_out = root/'build/pvr-bxm-media-v28'
    audit = root/'build/media-v28-candidate-build-20261117'
    stage = audit/'dcdc2-readback-addon'
    original = json.loads((audit/'build-result.json').read_text())
    candidate = root/'boards/candidates/media-v28-400mhz'
    manifest_path = candidate/'combined-manifest.json'
    manifest = json.loads(manifest_path.read_text())
    release = '6.18.63-t736-media-v28'
    assert original['release'] == release
    assert original['board_access'] is False
    assert len(original['source_changes']) == 2
    assert not stage.exists(), 'Do not overwrite a previous incremental attempt'
    assert not (audit/'build-result-combined.json').exists()
    assert sha(baseline_out/'.config') == original['baseline_config_sha256']
    for component in manifest['components']:
        assert sha(candidate/component['patch']) == component['patch_sha256']
    for item in manifest['files']:
        assert sha(baseline/item['kernel_path']) == item['baseline_sha256']
        assert sha(candidate/item['path']) == item['candidate_sha256']
        expected = (item['candidate_sha256'] if item['kernel_path'] in
                    original['source_changes'] else item['baseline_sha256'])
        assert sha(source/item['kernel_path']) == expected
    for rel, digest in original['protected_source_hashes'].items():
        assert sha(baseline/rel) == digest
        if rel not in original['source_changes']:
            assert sha(source/rel) == digest
    for artifact in original['artifacts']:
        assert sha(Path(artifact['path'])) == artifact['sha256']
    old_config_hash = sha(out/'.config')
    stage.mkdir()
    saved = stage/'initial-artifacts'
    saved.mkdir()
    for i, artifact in enumerate(original['artifacts']):
        old = Path(artifact['path'])
        target = saved/(f'{i:02d}-'+old.name)
        shutil.copy2(old, target)
        assert sha(target) == artifact['sha256']
    commands = []
    env = dict(os.environ, KBUILD_BUILD_VERSION='28')

    def run(name, command, *, extra_env=None):
        print(f'{name}: starting', flush=True)
        start = time.monotonic()
        command = [str(value) for value in command]
        with (stage/(name+'.log')).open('w') as stream:
            subprocess.run(command, env=dict(env, **(extra_env or {})),
                           stdout=stream, stderr=subprocess.STDOUT, check=True)
        commands.append({'stage': name, 'argv': command,
                         'elapsed_seconds': time.monotonic()-start})
        print(f'{name}: passed in {commands[-1]["elapsed_seconds"]:.1f}s', flush=True)

    addon = next(item for item in manifest['files'] if item['kernel_path'] not in
                 original['source_changes'])
    target = source/addon['kernel_path']
    assert target.stat().st_ino != (baseline/addon['kernel_path']).stat().st_ino
    # The candidate may predate the first build's object. Use a fresh mtime
    # so incremental make cannot reuse the object without the new callback.
    shutil.copyfile(candidate/addon['path'], target)
    os.utime(target, None)
    assert sha(target) == addon['candidate_sha256']
    make = ['make', '-C', source, f'O={out}', 'ARCH=arm64',
            'CROSS_COMPILE=aarch64-linux-gnu-']
    run('kernel-dtb-modules', make+[f'-j{args.jobs}', 'Image',
                                   'allwinner/t736-gpu-candidate.dtb', 'modules'])
    run('debian-package', make+[f'-j{args.jobs}', 'KDEB_PKGVERSION=6.18.63-28', 'bindeb-pkg'],
        extra_env={'DEB_BUILD_PROFILES': 'pkg.linux-upstream.nokernelheaders'})
    run('pvr-module', ['make', '-C', pvr_source/'build/linux/sunxi_linux', f'-j{args.jobs}',
                      f'KERNELDIR={out}', f'OUT={pvr_out}', 'ARCH=arm64',
                      'CROSS_COMPILE=aarch64-linux-gnu-', 'KERNEL_CC=aarch64-linux-gnu-gcc',
                      'KERNEL_LD=aarch64-linux-gnu-ld', 'KERNEL_NM=aarch64-linux-gnu-gcc-nm',
                      'KERNEL_OBJCOPY=aarch64-linux-gnu-objcopy', 'GPU_BUILD_TYPE=release',
                      'BUILD=release', 'SUPPORT_LINUX_DVFS=0', 'SUPPORT_PDVFS=0',
                      'SUPPORT_DMA_FENCE=1', 'SUPPORT_BUFFER_SYNC=1'])
    module = pvr_out/'target_aarch64/kbuild/pvrsrvkm.ko'
    info = subprocess.check_output(['aarch64-linux-gnu-readelf', '-p', '.modinfo', module], text=True)
    vermagic = re.search(r'vermagic=([^\n]+)', info).group(1).strip()
    assert vermagic.startswith(release+' ')
    dtb = out/'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb'
    assert sha(dtb) == sha(baseline_out/'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb')
    assert sha(out/'.config') == old_config_hash
    assert sha(baseline_out/'.config') == original['baseline_config_sha256']
    assert (out/'include/config/kernel.release').read_text().strip() == release
    for rel, digest in original['protected_source_hashes'].items():
        assert sha(baseline/rel) == digest
    changed = {item['kernel_path'] for item in manifest['files']}
    for rel, digest in original['protected_source_hashes'].items():
        if rel not in changed:
            assert sha(source/rel) == digest
    for item in manifest['files']:
        assert sha(source/item['kernel_path']) == item['candidate_sha256']
    package = root/'build'/f'linux-image-{release}_6.18.63-28_arm64.deb'
    fields = subprocess.check_output(['dpkg-deb', '-f', package,
                                     'Package', 'Version', 'Architecture', 'Depends'], text=True)
    assert f'Package: linux-image-{release}\n' in fields
    assert 'Version: 6.18.63-28\n' in fields and 'Architecture: arm64\n' in fields
    (stage/'package-fields.txt').write_text(fields)
    contents = subprocess.check_output(['dpkg-deb', '-c', package], text=True)
    (stage/'package-contents.txt').write_text(contents)
    assert f'./boot/vmlinuz-{release}' in contents
    assert f'./lib/modules/{release}/' in contents
    # The new observation interface must occur in the linked kernel, not only source.
    strings = subprocess.check_output(['aarch64-linux-gnu-strings', out/'vmlinux'], text=True)
    assert 'dcdc2_snapshot' in strings and 'kind=programmed-setpoint' in strings
    artifacts = [package, out/'arch/arm64/boot/Image', dtb, module, out/'.config', out/'Module.symvers']
    record = dict(original)
    record.update({'status': 'COMBINED_COMPILED_NOT_DEPLOYED_HARDWARE_PENDING',
                   'candidate_manifest_path': str(manifest_path),
                   'candidate_manifest': manifest,
                   'initial_build_record': str(audit/'build-result.json'),
                   'initial_artifacts_saved': str(saved),
                   'commands': original['commands']+commands,
                   'source_changes': sorted(changed),
                   'pvr_vermagic': vermagic,
                   'dcdc2_readonly_linked': True,
                   'package_fields': fields,
                   'artifacts': [{'path': str(path), 'bytes': path.stat().st_size,
                                  'sha256': sha(path)} for path in artifacts],
                   'limitations': ['Not deployed; DCDC2 setpoint must be read before decoder testing.',
                                   'Readback is a programmed setpoint, not an ADC measurement.',
                                   '400 MHz throughput and stability remain unverified.',
                                   'DMA lifetime and 128 MiB quarantine remain unchanged.']})
    (audit/'build-result-combined.json').write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps(record['artifacts']), flush=True)


if __name__ == '__main__':
    main()
