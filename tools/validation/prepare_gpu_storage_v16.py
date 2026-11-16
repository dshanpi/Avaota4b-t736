#!/usr/bin/env python3
"""Generate ABI-specific v16 helpers without changing the validated v15 sources."""
import argparse
import ast
import hashlib
import json
from pathlib import Path

OLD = '6.18.63-t736-gpu-v15'
NEW = '6.18.63-t736-storage-v16'


def generate(root, output):
    root = Path(root).resolve(strict=True)
    output = Path(output)
    if output.exists() or output.is_symlink():
        raise RuntimeError('Output directory must be new')
    files = {}
    sources = {}
    for source, name in (
        ('tools/validation/gpu_profile.py', 't736-gpu-profile'),
        ('tools/validation/gpu_runtime_check.sh', 't736-gpu-runtime-check'),
    ):
        data = (root / source).read_bytes()
        text = data.decode()
        if text.count(OLD) != 1:
            raise RuntimeError('Expected exactly one release guard in ' + source)
        changed = text.replace(OLD, NEW)
        if name == 't736-gpu-profile':
            ast.parse(changed)
        else:
            changed = changed.replace('# Run only on the v15 candidate;', '# Run only on the storage-v16 candidate;')
        files[name] = changed.encode()
        sources[source] = hashlib.sha256(data).hexdigest()
    source = 'config/gpu-v9/acceptance.template.json'
    template = (root / source).read_bytes()
    accepted = json.loads(template.replace(OLD.encode(), NEW.encode()))
    if accepted['status'] != 'pending' or any(accepted['checks'].values()):
        raise RuntimeError('Template must not contain acceptance claims')
    files['acceptance.template.json'] = (json.dumps(accepted, indent=2) + '\n').encode()
    sources[source] = hashlib.sha256(template).hexdigest()
    for source, name in (
        ('config/gpu-v10/20-t736-modesetting-1080p.conf', '20-t736-modesetting-1080p.conf'),
        ('tools/validation/mutter_set_1080p.py', 't736-mutter-1080p'),
    ):
        files[name] = (root / source).read_bytes()
        sources[source] = hashlib.sha256(files[name]).hexdigest()
    record = {
        'schema': 1, 'kernel_release': NEW, 'hardware_accepted': False,
        'source_sha256': sources,
        'files': {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                  for name, data in files.items()},
        'changes': ['Profile release constant and runtime uname guard changed from v15 to v16',
                    'Runtime comment updated; profile logic, 1080p config and Mutter helper unchanged',
                    'Acceptance template remains pending with all checks false'],
        'excluded_legacy_tools': ['gpu_session_trial.sh', 't736-a76-clock-trial.ko'],
    }
    output.mkdir(parents=True)
    for name, data in files.items():
        (output / name).write_bytes(data)
    (output / 'manifest.json').write_bytes((json.dumps(record, indent=2) + '\n').encode())
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(generate(args.root, args.output), indent=2))


if __name__ == '__main__':
    main()
