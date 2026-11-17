#!/usr/bin/env python3
"""Build isolated v29 with madvise enabled; never deploy or modify v28/v16."""
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
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('/home/ubuntu/t736-mainline'))
    parser.add_argument('--jobs', type=int, default=12)
    parser.add_argument('--resume-config-only', action='store_true',
                        help='Resume only the known pre-compilation config audit attempt')
    args = parser.parse_args()
    root = args.root.resolve()
    source = root/'sources/work/linux-media-v28'  # Read-only shared source, new O=.
    old_out = root/'build/linux-media-v28'
    out = root/'build/linux-media-v29'
    old_pvr_source = root/'sources/work/pvr-bxm-media-v28-24.2'
    pvr_source = root/'sources/work/pvr-bxm-media-v29-24.2'
    pvr_out = root/'build/pvr-bxm-media-v29'
    audit = root/'build/media-v29-candidate-build-20261117'
    previous_record = root/'build/media-v28-candidate-build-20261117/build-result-combined.json'
    previous = json.loads(previous_record.read_text(encoding='utf-8'))
    release = '6.18.63-t736-media-v29'
    package = root/'build'/f'linux-image-{release}_6.18.63-29_arm64.deb'
    if args.resume_config_only:
        assert out.is_dir() and pvr_source.is_dir() and audit.is_dir()
        assert not (out/'arch/arm64/boot/Image').exists()
        assert not pvr_out.exists() and not package.exists()
        assert not (audit/'build-result.json').exists()
        saved = audit/'initial-config-audit'
        assert not saved.exists()
        saved.mkdir()
        for log in audit.glob('*.log'):
            shutil.copy2(log, saved/log.name)
        shutil.copy2(out/'.config', saved/'.config')
    else:
        for path in (out, pvr_source, pvr_out, audit, package):
            if path.exists():
                raise RuntimeError(f'New isolated output required; path exists: {path}')
    if shutil.disk_usage(root).free < 10 * 1024**3:
        raise RuntimeError('Less than10 GiB available for isolated build')
    assert previous['release'] == '6.18.63-t736-media-v28'
    assert (old_out/'include/config/kernel.release').read_text().strip() == previous['release']
    assert len(previous['source_changes']) == 3
    for artifact in previous['artifacts']:
        assert sha(Path(artifact['path'])) == artifact['sha256'], artifact['path']
    inherited = previous['candidate_manifest']['files']
    for item in inherited:
        assert sha(source/item['kernel_path']) == item['candidate_sha256']
    protected = {str(path.relative_to(source)): sha(path)
                 for path in source.rglob('*t736*') if path.is_file()}
    protected['Makefile'] = sha(source/'Makefile')
    protected['init/Kconfig'] = sha(source/'init/Kconfig')
    protected['mm/madvise.c'] = sha(source/'mm/madvise.c')
    protected['mm/fadvise.c'] = sha(source/'mm/fadvise.c')
    pvr_protected = {str(path.relative_to(old_pvr_source)): sha(path)
                     for path in old_pvr_source.rglob('*') if path.is_file()}
    old_config = (old_out/'.config').read_text()
    assert '# CONFIG_ADVISE_SYSCALLS is not set' in old_config.splitlines()
    audit.mkdir(parents=True, exist_ok=args.resume_config_only)
    commands = []
    env = dict(os.environ, KBUILD_BUILD_VERSION='29')

    def run(name, command, *, extra_env=None):
        print(f'{name}: starting', flush=True)
        start = time.monotonic()
        command = [str(value) for value in command]
        with (audit/(name+'.log')).open('w', encoding='utf-8') as log:
            subprocess.run(command, env=dict(env, **(extra_env or {})),
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        commands.append({'stage': name, 'argv': command,
                         'elapsed_seconds': time.monotonic() - start})
        print(f'{name}: passed in {commands[-1]["elapsed_seconds"]:.1f}s', flush=True)

    # The DDK has its own generated build state; give it an independent source
    # copy as well as output. The Linux source remains the verified v28 tree.
    if not args.resume_config_only:
        run('copy-pvr', ['cp', '-a', '--reflink=auto', '--', old_pvr_source, pvr_source])
    for rel, digest in pvr_protected.items():
        assert sha(pvr_source/rel) == digest, rel
    out.mkdir(exist_ok=args.resume_config_only)
    shutil.copy2(old_out/'.config', out/'.config')
    run('set-candidate-config', [source/'scripts/config', '--file', out/'.config',
        '--set-str', 'LOCALVERSION', '-t736-media-v29', '--enable', 'ADVISE_SYSCALLS'])
    make = ['make', '-C', source, f'O={out}', 'ARCH=arm64',
            'CROSS_COMPILE=aarch64-linux-gnu-']
    run('olddefconfig', make + ['olddefconfig'])
    old_lines, new_lines = set(old_config.splitlines()), set((out/'.config').read_text().splitlines())
    removed = old_lines - new_lines
    added = new_lines - old_lines
    assert removed == {'CONFIG_LOCALVERSION="-t736-media-v28"',
                       '# CONFIG_ADVISE_SYSCALLS is not set'}, removed
    # ANON_VMA_NAME depends on ADVISE_SYSCALLS. It was hidden and disabled in
    # v28; olddefconfig now writes its explicit disabled comment. It stays off.
    assert added == {'CONFIG_LOCALVERSION="-t736-media-v29"',
                     'CONFIG_ADVISE_SYSCALLS=y',
                     '# CONFIG_ANON_VMA_NAME is not set'}, added
    assert 'CONFIG_ANON_VMA_NAME' not in (old_out/'include/generated/autoconf.h').read_text()
    def semantic(lines):
        values = {}
        for line in lines:
            if line.startswith('CONFIG_'):
                key, value = line.split('=', 1)
                values[key] = value
        return values
    old_values, new_values = semantic(old_lines), semantic(new_lines)
    semantic_diff = {key: {'old': old_values.get(key, 'n'), 'new': new_values.get(key, 'n')}
                     for key in old_values.keys() | new_values.keys()
                     if old_values.get(key, 'n') != new_values.get(key, 'n')}
    assert set(semantic_diff) == {'CONFIG_LOCALVERSION', 'CONFIG_ADVISE_SYSCALLS'}
    config_diff = {'removed': sorted(removed), 'added': sorted(added),
                   'semantic_changes': semantic_diff,
                   'disabled_comment_note': 'ANON_VMA_NAME remains disabled; its dependency became visible.'}
    (audit/'config-diff.json').write_text(json.dumps(config_diff, indent=2)+'\n')
    run('kernel-dtb-modules', make + [f'-j{args.jobs}', 'Image',
                                    'allwinner/t736-gpu-candidate.dtb', 'modules'])
    assert (out/'include/config/kernel.release').read_text().strip() == release
    assert 'CONFIG_ANON_VMA_NAME' not in (out/'include/generated/autoconf.h').read_text()
    run('debian-package', make + [f'-j{args.jobs}', 'KDEB_PKGVERSION=6.18.63-29', 'bindeb-pkg'],
        extra_env={'DEB_BUILD_PROFILES': 'pkg.linux-upstream.nokernelheaders'})
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
    assert sha(dtb) == sha(old_out/'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb')
    symbols = subprocess.check_output(['aarch64-linux-gnu-nm', out/'vmlinux'], text=True)
    expected_symbols = ['__arm64_sys_madvise', '__arm64_sys_fadvise64_64']
    for symbol in expected_symbols:
        assert re.search(r'\b[Tt]\s+' + re.escape(symbol) + r'$', symbols, re.MULTILINE), symbol
    (audit/'advise-symbols.txt').write_text('\n'.join(
        line for line in symbols.splitlines() if any(line.endswith(' '+s) for s in expected_symbols))+'\n')
    strings = subprocess.check_output(['aarch64-linux-gnu-strings', out/'vmlinux'], text=True)
    assert 'dcdc2_snapshot' in strings and 'kind=programmed-setpoint' in strings
    fields = subprocess.check_output(['dpkg-deb', '-f', package,
                                     'Package', 'Version', 'Architecture', 'Depends'], text=True)
    assert f'Package: linux-image-{release}\n' in fields
    assert 'Version: 6.18.63-29\n' in fields and 'Architecture: arm64\n' in fields
    (audit/'package-fields.txt').write_text(fields)
    contents = subprocess.check_output(['dpkg-deb', '-c', package], text=True)
    (audit/'package-contents.txt').write_text(contents)
    assert f'./boot/vmlinuz-{release}' in contents
    assert f'./lib/modules/{release}/' in contents
    for artifact in previous['artifacts']:
        assert sha(Path(artifact['path'])) == artifact['sha256'], artifact['path']
    for rel, digest in protected.items():
        assert sha(source/rel) == digest, rel
    for rel, digest in pvr_protected.items():
        assert sha(old_pvr_source/rel) == digest, rel
    for item in inherited:
        assert sha(source/item['kernel_path']) == item['candidate_sha256']
    final_config = set((out/'.config').read_text().splitlines())
    assert old_lines - final_config == removed and final_config - old_lines == added
    artifacts = [package, out/'arch/arm64/boot/Image', dtb, module,
                 out/'.config', out/'Module.symvers']
    record = {'schema': 1, 'status': 'COMPILED_NOT_DEPLOYED_SYSCALL_AND_SHM_BOARD_TEST_PENDING',
        'release': release, 'board_access': False, 'deployment': False,
        'builder_sha256': sha(Path(__file__)), 'baseline_record': str(previous_record),
        'baseline_record_sha256': sha(previous_record), 'baseline_artifacts': previous['artifacts'],
        'source_directory_reused_readonly': str(source), 'source_changes': [],
        'inherited_v28_three_source_files': inherited, 'protected_source_hashes': protected,
        'protected_v28_pvr_source_hashes': pvr_protected, 'config_diff': config_diff,
        'configuration_change_only': ['CONFIG_LOCALVERSION', 'CONFIG_ADVISE_SYSCALLS'],
        'dtb_equal_to_v28': True, 'v28_sources_pvr_config_artifacts_unchanged': True,
        'default_storage_v16_untouched': True, 'dcdc2_readonly_linked': True,
        'linked_advise_symbols': expected_symbols, 'pvr_vermagic': vermagic,
        'commands': commands, 'package_fields': fields,
        'artifacts': [{'path': str(p), 'bytes': p.stat().st_size, 'sha256': sha(p)} for p in artifacts],
        'limitations': ['Enabling madvise/fadvise removes the observed ENOSYS cause; real board tests still required.',
                       'No claim of improved video performance until SHM v2 hardware comparison.',
                       'VE400MHz, DCDC2 read-only interface, GPU behavior, DMA quarantine and boot defaults unchanged.']}
    (audit/'build-result.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(record['artifacts']), flush=True)


if __name__ == '__main__':
    main()
