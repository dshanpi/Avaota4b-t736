#!/usr/bin/env python3
"""Read-only build-host audit of the isolated, madvise-enabled v29 package."""
import argparse
import difflib
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import subprocess
import tarfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def archive(data):
    manifest, contents = {}, {}
    with tarfile.open(fileobj=io.BytesIO(data)) as stream:
        for item in stream:
            name = item.name
            while name.startswith('./'):
                name = name[2:]
            path = PurePosixPath(name)
            assert not path.is_absolute() and '..' not in path.parts, name
            name = str(path)
            if item.isdir():
                continue
            assert name != '.' and name not in manifest, name
            assert item.isfile() or item.issym(), name
            entry = {'mode': item.mode & 0o7777, 'type': 'file' if item.isfile() else 'symlink'}
            if item.isfile():
                contents[name] = stream.extractfile(item).read()
                entry.update(sha256=digest(contents[name]), bytes=item.size)
            else:
                entry['target'] = item.linkname
            manifest[name] = entry
    return manifest, contents


def manifest_digest(value):
    return digest(json.dumps(value, sort_keys=True, separators=(',', ':')).encode())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('/home/ubuntu/t736-mainline'))
    args = parser.parse_args()
    root = args.root.resolve()
    new, old = '6.18.63-t736-media-v29', '6.18.63-t736-media-v28'
    record_path = root/'build/media-v29-candidate-build-20261117/build-result.json'
    record = json.loads(record_path.read_bytes())
    old_audit_path = root/'evidence/media-v28-package-stage-audit-20261117.json'
    assert digest(old_audit_path.read_bytes()) == '51fd544b5ca1c9930e8c2a5cf45ed3dfa38cfd2538bea965a6d66abe0e3173f2'
    old_audit = json.loads(old_audit_path.read_bytes())
    for item in record['artifacts'] + record['baseline_artifacts']:
        path = Path(item['path'])
        assert path.is_relative_to(root) and path.is_file() and not path.is_symlink()
        assert path.stat().st_size == item['bytes'] and digest(path.read_bytes()) == item['sha256'], str(path)
    package = root/'build'/f'linux-image-{new}_6.18.63-29_arm64.deb'
    module = root/'build/pvr-bxm-media-v29/target_aarch64/kbuild/pvrsrvkm.ko'
    raw_image = root/'build/linux-media-v29/arch/arm64/boot/Image'
    dtb_path = root/'build/linux-media-v29/arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb'
    fields = subprocess.check_output(['dpkg-deb', '-f', package, 'Package', 'Version', 'Architecture']).decode().splitlines()
    assert fields == ['Package: linux-image-' + new, 'Version: 6.18.63-29', 'Architecture: arm64']
    control, control_bytes = archive(subprocess.check_output(['dpkg-deb', '--ctrl-tarfile', package]))
    payload, payload_bytes = archive(subprocess.check_output(['dpkg-deb', '--fsys-tarfile', package]))
    assert set(control) == {'control', 'md5sums', 'postinst', 'postrm', 'preinst', 'prerm'}
    scripts = {}
    for name in ('postinst', 'postrm', 'preinst', 'prerm'):
        scripts[name] = control_bytes[name].decode()
        assert scripts[name] == old_audit['maintainer_scripts'][name].replace(old, new), name
    for name in payload:
        assert (name in ('boot/vmlinuz-' + new, 'boot/config-' + new, 'boot/System.map-' + new)
                or name.startswith('lib/modules/' + new + '/')
                or name.startswith('usr/lib/modules/' + new + '/')
                or name.startswith('usr/lib/linux-image-' + new + '/')
                or name.startswith('usr/share/doc/linux-image-' + new + '/')), name
    config = payload_bytes['boot/config-' + new]
    old_config = (root/'build/linux-media-v28/.config').read_bytes()
    assert digest(old_config) == old_audit['payload']['boot/config-' + old]['sha256']
    assert config == (root/'build/linux-media-v29/.config').read_bytes()
    old_lines, new_lines = set(old_config.decode().splitlines()), set(config.decode().splitlines())
    assert old_lines-new_lines == {'CONFIG_LOCALVERSION="-t736-media-v28"', '# CONFIG_ADVISE_SYSCALLS is not set'}
    assert new_lines-old_lines == {'CONFIG_LOCALVERSION="-t736-media-v29"', 'CONFIG_ADVISE_SYSCALLS=y', '# CONFIG_ANON_VMA_NAME is not set'}
    assert config.splitlines().count(b'CONFIG_ADVISE_SYSCALLS=y') == 1
    for version in ('v28', 'v29'):
        assert 'CONFIG_ANON_VMA_NAME' not in (root/f'build/linux-media-{version}/include/generated/autoconf.h').read_text()
    raw = gzip.decompress(payload_bytes['boot/vmlinuz-' + new])
    assert raw == raw_image.read_bytes()
    dtb = payload_bytes['usr/lib/linux-image-' + new + '/allwinner/t736-gpu-candidate.dtb']
    assert dtb == dtb_path.read_bytes() and digest(dtb) == old_audit['candidate_dtb_sha256']
    assert subprocess.check_output(['modinfo', '-F', 'name', module]).decode().strip() == 'pvrsrvkm'
    vermagic = subprocess.check_output(['modinfo', '-F', 'vermagic', module]).decode().strip()
    assert vermagic.startswith(new + ' ')
    assert b'Rogue_DDK_Linux_WS rogueddk 24.2@6603887' in module.read_bytes()
    symbols = subprocess.check_output(['aarch64-linux-gnu-nm', root/'build/linux-media-v29/vmlinux']).decode().splitlines()
    linked = [line for line in symbols if line.endswith((' T __arm64_sys_madvise', ' T __arm64_sys_fadvise64_64'))]
    assert len(linked) == 2, linked
    evidence = {'schema': 1, 'status': 'READ_ONLY_PACKAGE_AUDIT_NO_BOARD_ACCESS',
                'package_metadata': fields, 'build_record_sha256': digest(record_path.read_bytes()),
                'baseline_audit_sha256': digest(old_audit_path.read_bytes()),
                'audit_tool_sha256': digest(Path(__file__).read_bytes()),
                'control_manifest_sha256': manifest_digest(control),
                'payload_manifest_sha256': manifest_digest(payload),
                'control': control, 'payload': payload, 'maintainer_scripts': scripts,
                'maintainer_scripts_equal_v28_except_release': True,
                'config_diff': list(difflib.unified_diff(old_config.decode().splitlines(), config.decode().splitlines(), fromfile=old, tofile=new, lineterm='')),
                'semantic_config_changes': ['CONFIG_LOCALVERSION', 'CONFIG_ADVISE_SYSCALLS'],
                'anon_vma_name_disabled_in_both_generated_autoconf': True,
                'dtb_equal_to_v28': True, 'packaged_gzip_equals_build_Image': True,
                'linked_advise_symbols': linked, 'pvr_vermagic': vermagic,
                'limitations': ['Read-only package audit; no staging, reboot, syscall runtime or SHM performance acceptance.']}
    for key, path in (('kernel', package), ('pvr', module), ('raw_image', raw_image), ('candidate_dtb', dtb_path)):
        evidence[key + '_bytes'] = path.stat().st_size
        evidence[key + '_sha256'] = digest(path.read_bytes())
    output = root/'evidence/media-v29-package-stage-audit-20261117.json'
    with output.open('x', encoding='utf-8') as stream:
        json.dump(evidence, stream, indent=2)
        stream.write('\n')
    output.chmod(0o600)
    print(json.dumps({key: value for key, value in evidence.items() if key.endswith(('_sha256', '_bytes'))}, indent=2))
    print('audit_sha256=' + digest(output.read_bytes()))


if __name__ == '__main__':
    main()
