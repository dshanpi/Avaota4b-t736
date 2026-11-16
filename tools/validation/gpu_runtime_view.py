#!/usr/bin/env python3
"""Create/verify a loader view without changing the private IMG payload."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

PREFIX = '/opt/t736-img-ddk-24.2-x11'
VIEW = 'runtime-gles'
REQUIRED = {'libEGL.so.1', 'libGLESv2.so.2', 'libgbm.so.1', 'libglapi.so.0', 'dri'}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected_view(private):
    lib = private / 'lib'
    require(lib.is_dir() and not lib.is_symlink(), 'Expected an original lib directory')
    require((private / 'manifest.json').is_file(), 'Missing original payload manifest')
    links, excluded = {}, []
    for entry in sorted(lib.iterdir()):
        resolved = entry.resolve(strict=True)
        require(resolved.is_relative_to(lib.resolve()), 'Payload link escapes lib: ' + str(entry))
        require(resolved.is_file() or resolved.is_dir(), 'Unsupported payload entry: ' + str(entry))
        if entry.name.startswith('libvulkan.so') or resolved.name.startswith('libvulkan.so'):
            excluded.append(entry.name)
        else:
            links[entry.name] = '../lib/' + entry.name
    require(REQUIRED <= links.keys(), 'Incomplete EGL/GLES/GBM/DRI payload')
    require(excluded, 'No vendor Vulkan loader found; payload needs a fresh review')
    return {
        'schema': 1,
        'prefix': PREFIX,
        'view': VIEW,
        'payload_manifest_sha256': digest(private / 'manifest.json'),
        'links': links,
        'excluded': excluded,
        'vulkan_loader': 'system default search; vendor ICD is not enabled by this helper',
    }


def verify_links(view, expected):
    require(view.is_dir() and not view.is_symlink(), 'Expected a real runtime view directory')
    require({p.name for p in view.iterdir()} == set(expected['links']),
            'Runtime view entries differ from the reviewed payload mapping')
    for name, target in expected['links'].items():
        link = view / name
        require(link.is_symlink() and str(link.readlink()) == target,
                'Unexpected runtime view link: ' + str(link))
        require(link.exists(), 'Broken runtime view link: ' + str(link))


def prepare(root, verify=False):
    root = Path(root).resolve(strict=True)
    private = root / PREFIX.lstrip('/')
    require(private.is_dir() and not private.is_symlink(), 'Missing original private payload')
    expected = expected_view(private)
    view = private / VIEW
    record = private / (VIEW + '.json')
    data = (json.dumps(expected, sort_keys=True, indent=2) + '\n').encode()
    if verify:
        verify_links(view, expected)
        require(record.is_file() and not record.is_symlink() and record.read_bytes() == data,
                'Runtime view provenance differs or is absent')
    else:
        if not view.exists() and not view.is_symlink():
            temporary = Path(tempfile.mkdtemp(prefix='.' + VIEW + '-', dir=private))
            try:
                for name, target in expected['links'].items():
                    (temporary / name).symlink_to(target)
                verify_links(temporary, expected)
                temporary.rename(view)
            finally:
                if temporary.exists():
                    for link in temporary.iterdir():
                        link.unlink()
                    temporary.rmdir()
        verify_links(view, expected)
        if record.exists() or record.is_symlink():
            require(not record.is_symlink() and record.read_bytes() == data,
                    'Existing runtime view provenance differs; refusing replacement')
        else:
            with record.open('xb') as stream:
                stream.write(data)
        os.chmod(view, 0o755)
        os.chmod(record, 0o644)
    return {'runtime_path': PREFIX + '/' + VIEW,
            'manifest_path': PREFIX + '/' + VIEW + '.json',
            'manifest_sha256': hashlib.sha256(data).hexdigest(),
            'links': len(expected['links']), 'excluded': expected['excluded']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default='/', help='Installed or offline rootfs')
    parser.add_argument('--verify', action='store_true', help='Check existing view without changes')
    args = parser.parse_args()
    print(json.dumps(prepare(args.root, args.verify), indent=2))


if __name__ == '__main__':
    main()
