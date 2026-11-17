#!/usr/bin/env python3
"""Linux host-only rejection tests for a wrongly bound v29 madvise config."""
import argparse
import copy
import importlib.util
import io
import json
from pathlib import Path
import tarfile
from unittest.mock import patch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--updater', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--pvr', type=Path, required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('stage', args.updater)
    stage = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(stage)
    stage.inputs(args.kernel, args.pvr)
    checks = ['actual fixed package accepts its enabled madvise config']
    actual_run = stage.run
    original = actual_run(['dpkg-deb', '--fsys-tarfile', args.kernel])
    config_name = 'boot/config-' + stage.NEW
    cases = {
        'missing setting': b'CONFIG_CMA_SIZE_MBYTES=256\n',
        'disabled setting': b'# CONFIG_ADVISE_SYSCALLS is not set\n',
        'explicit n': b'CONFIG_ADVISE_SYSCALLS=n\n',
        'conflicting duplicate': b'CONFIG_ADVISE_SYSCALLS=y\n# CONFIG_ADVISE_SYSCALLS is not set\n',
        'duplicate y': b'CONFIG_ADVISE_SYSCALLS=y\nCONFIG_ADVISE_SYSCALLS=y\n',
        'missing config file': None,
        'config symlink': 'symlink',
    }
    for name, replacement in cases.items():
        output = io.BytesIO()
        with tarfile.open(fileobj=io.BytesIO(original)) as source, \
             tarfile.open(fileobj=output, mode='w') as target:
            for member in source:
                item = copy.copy(member)
                data = source.extractfile(member).read() if member.isfile() else None
                if stage.clean_name(member.name) == config_name:
                    if replacement is None:
                        continue
                    if replacement == 'symlink':
                        item.type = tarfile.SYMTYPE
                        item.linkname = '/tmp/untrusted-config'
                        item.size = 0
                        data = None
                    else:
                        data = replacement
                        item.size = len(data)
                target.addfile(item, io.BytesIO(data) if data is not None else None)
        changed = output.getvalue()
        # Simulate an independently signed/bound but unsuitable payload. The
        # semantic precheck must reject it even when its manifest hash matches.
        binding = dict(stage.BINDINGS,
                       payload_manifest_sha256=stage.manifest_sha(stage.archive_manifest(changed)))

        def run(argv, *a, **kw):
            if argv[:2] == ['dpkg-deb', '--fsys-tarfile']:
                return changed
            if argv[0] != 'dpkg-deb':
                raise AssertionError('Bad config escaped precheck into ' + str(argv))
            return actual_run(argv, *a, **kw)

        with patch.object(stage, 'BINDINGS', binding), patch.object(stage, 'run', run):
            try:
                stage.inputs(args.kernel, args.pvr)
                raise AssertionError(name + ' was accepted')
            except RuntimeError as error:
                if 'CONFIG_ADVISE_SYSCALLS' not in str(error) and 'config missing' not in str(error):
                    raise
                checks.append(name + ' rejected during inputs before module/board access')
    print(json.dumps({'status': 'PASS_HOST_ONLY', 'board_access': False, 'checks': checks}, indent=2))


if __name__ == '__main__':
    main()
