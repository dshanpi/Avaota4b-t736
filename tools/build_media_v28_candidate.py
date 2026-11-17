#!/usr/bin/env python3
"""Build the isolated VE 400 MHz candidate; never deploy or edit v27/v16."""
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
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('/home/ubuntu/t736-mainline'))
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    root = args.root.resolve()
    baseline = root/'sources/work/linux-media-v27'
    baseline_out = root/'build/linux-media-v27'
    source = root/'sources/work/linux-media-v28'
    out = root/'build/linux-media-v28'
    pvr_base = root/'sources/work/pvr-bxm-24.2'
    pvr_source = root/'sources/work/pvr-bxm-media-v28-24.2'
    pvr_out = root/'build/pvr-bxm-media-v28'
    audit = root/'build/media-v28-candidate-build-20261117'
    candidate = root/'boards/candidates/media-v28-400mhz'
    release = '6.18.63-t736-media-v28'
    manifest_path = candidate/'combined-manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    for path in (source, out, pvr_source, pvr_out, audit):
        if path.exists():
            raise RuntimeError(f'New isolated output required; path already exists: {path}')
    if shutil.disk_usage(root).free < 10 * 1024**3:
        raise RuntimeError('Less than 10 GiB available for isolated build')
    assert (baseline_out/'include/config/kernel.release').read_text().strip() == '6.18.63-t736-media-v27'
    baseline_config_sha = sha(baseline_out/'.config')
    for item in manifest['files']:
        assert sha(baseline/item['kernel_path']) == item['baseline_sha256']
        assert sha(candidate/item['path']) == item['candidate_sha256']
    for component in manifest['components']:
        assert sha(candidate/component['patch']) == component['patch_sha256']
    protected = {str(p.relative_to(baseline)): sha(p)
                 for p in baseline.rglob('*t736*') if p.is_file()}
    audit.mkdir(parents=True)
    commands = []
    env = dict(os.environ, KBUILD_BUILD_VERSION='28')

    def run(name, command, *, cwd=None, extra_env=None):
        print(f'{name}: starting', flush=True)
        start = time.monotonic()
        command = [str(x) for x in command]
        with (audit/(name+'.log')).open('w', encoding='utf-8') as log:
            subprocess.run(command, cwd=cwd, env=dict(env, **(extra_env or {})),
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        commands.append({'stage': name, 'argv': command, 'elapsed_seconds': time.monotonic()-start})
        print(f'{name}: passed in {commands[-1]["elapsed_seconds"]:.1f}s', flush=True)

    run('copy-kernel', ['cp', '-a', '--reflink=auto', '--', baseline, source])
    run('copy-pvr', ['cp', '-a', '--reflink=auto', '--', pvr_base, pvr_source])
    for item in manifest['files']:
        original, new = baseline/item['kernel_path'], source/item['kernel_path']
        assert original.stat().st_ino != new.stat().st_ino
        shutil.copy2(candidate/item['path'], new)
        assert sha(new) == item['candidate_sha256']
    changed = {item['kernel_path'] for item in manifest['files']}
    for rel, digest in protected.items():
        if rel not in changed:
            assert sha(source/rel) == digest, rel
    out.mkdir()
    shutil.copy2(baseline_out/'.config', out/'.config')
    run('set-localversion', [source/'scripts/config', '--file', out/'.config',
                            '--set-str', 'LOCALVERSION', '-t736-media-v28'])
    make = ['make', '-C', source, f'O={out}', 'ARCH=arm64', 'CROSS_COMPILE=aarch64-linux-gnu-']
    run('olddefconfig', make+['olddefconfig'])
    old = set((baseline_out/'.config').read_text().splitlines())
    new = set((out/'.config').read_text().splitlines())
    assert old-new == {'CONFIG_LOCALVERSION="-t736-media-v27"'}, old-new
    assert new-old == {'CONFIG_LOCALVERSION="-t736-media-v28"'}, new-old
    run('kernel-dtb-modules', make+[f'-j{args.jobs}', 'Image',
                                   'allwinner/t736-gpu-candidate.dtb', 'modules'])
    assert (out/'include/config/kernel.release').read_text().strip() == release
    run('debian-package', make+[f'-j{args.jobs}', 'KDEB_PKGVERSION=6.18.63-28', 'bindeb-pkg'],
        extra_env={'DEB_BUILD_PROFILES': 'pkg.linux-upstream.nokernelheaders'})
    # Use a separate DDK source tree as well as a new output directory.
    kbuild = pvr_out/'target_aarch64/kbuild'
    for path in pvr_source.rglob('*.c'):
        rel = path.relative_to(pvr_source)
        if rel.parts[0] == 'build':
            continue
        link = kbuild/rel
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(path)
    run('pvr-module', ['make', '-C', pvr_source/'build/linux/sunxi_linux', f'-j{args.jobs}',
                      f'KERNELDIR={out}', f'OUT={pvr_out}', 'ARCH=arm64',
                      'CROSS_COMPILE=aarch64-linux-gnu-', 'KERNEL_CC=aarch64-linux-gnu-gcc',
                      'KERNEL_LD=aarch64-linux-gnu-ld', 'KERNEL_NM=aarch64-linux-gnu-gcc-nm',
                      'KERNEL_OBJCOPY=aarch64-linux-gnu-objcopy', 'GPU_BUILD_TYPE=release',
                      'BUILD=release', 'SUPPORT_LINUX_DVFS=0', 'SUPPORT_PDVFS=0',
                      'SUPPORT_DMA_FENCE=1', 'SUPPORT_BUFFER_SYNC=1'])
    module = kbuild/'pvrsrvkm.ko'
    info = subprocess.check_output(['aarch64-linux-gnu-readelf', '-p', '.modinfo', module], text=True)
    vermagic = re.search(r'vermagic=([^\n]+)', info).group(1).strip()
    assert vermagic.startswith(release+' '), vermagic
    dtb = out/'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb'
    assert sha(dtb) == sha(baseline_out/'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb')
    for item in manifest['files']:
        assert sha(baseline/item['kernel_path']) == item['baseline_sha256']
    assert sha(baseline_out/'.config') == baseline_config_sha
    for rel, digest in protected.items():
        assert sha(baseline/rel) == digest, rel
    package = root/'build'/f'linux-image-{release}_6.18.63-28_arm64.deb'
    artifacts = [package, out/'arch/arm64/boot/Image', dtb, module, out/'.config', out/'Module.symvers']
    record = {'schema': 1, 'status': 'COMPILED_NOT_DEPLOYED_HARDWARE_PENDING',
              'release': release, 'board_access': False,
              'candidate_manifest_path': str(manifest_path),
              'candidate_manifest': manifest, 'commands': commands,
              'baseline_config_sha256': baseline_config_sha,
              'source_changes': sorted(changed), 'protected_source_hashes': protected,
              'configuration_change_only': 'CONFIG_LOCALVERSION', 'dtb_equal_to_v27': True,
              'v27_sources_and_config_unchanged': True, 'pvr_vermagic': vermagic,
              'artifacts': [{'path': str(p), 'bytes': p.stat().st_size, 'sha256': sha(p)} for p in artifacts],
              'limitations': ['No board deployment or actual clock/voltage validation.',
                             'Frequency experiment does not establish 1080p60 playback performance.',
                             'DMA lifetime and 128 MiB quarantine are unchanged.']}
    (audit/'build-result.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(record['artifacts']), flush=True)


if __name__ == '__main__':
    main()
