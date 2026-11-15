#!/usr/bin/env python3
"""Prepare and verify the reviewed source/evidence backup; never push or flash."""
import argparse
import hashlib
import json
import os
import re
import subprocess
from datetime import datetime, timezone, timedelta
from pathlib import Path

SOURCE_DIRS = ('boards', 'config', 'docs', 'handoff', 'initramfs', 'patches',
               'spl', 'tests', 'tools', 'userpatches')
TEXT_SUFFIXES = {'', '.c', '.h', '.S', '.ld', '.py', '.sh', '.json', '.md',
                 '.patch', '.config', '.fex', '.wip', '.conf', '.dtsi', '.dts'}
EVIDENCE_SUFFIXES = {'.json', '.log', '.md', '.txt'}
EVIDENCE_PREFIXES = ('source-', 'vendor-ddr-', 't736-', 'display-', 'board-power-',
                     'lpddr4-', 'independent-', 'u-boot-source-', 'tfa-', 'open-pmu-')
OMIT_EVIDENCE = ('board-capture-', 'lynx-tools-', 'armbian-password-', 'source-ssh-identity-')
OMIT_PATH_PREFIXES = ('evidence/vendor-reference/', 'evidence/independent-reference/')
ANSI = re.compile(r'\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07)|[@-_])')
SECRET = re.compile(rb'-----BEGIN (?:OPENSSH|RSA|EC|DSA) PRIVATE KEY|'
                    rb'gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|'
                    rb'\$(?:y|6|5|2b)\$[A-Za-z0-9./$]{15,}')


def git(target, *args):
    return subprocess.check_output(['git', '-C', str(target), *args], text=True).strip()


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n',
                    encoding='utf-8', newline='\n')


def known_private_values():
    # Optional local guard. Never print, store, copy or upload these values.
    temp = os.environ.get('TEMP')
    p = Path(temp) / 't736-new-root-credential.json' if temp else None
    if p and p.is_file():
        data = json.loads(p.read_text(encoding='utf-8'))
        return [data[k].encode() for k in ('password', 'hash')
                if isinstance(data.get(k), str) and len(data[k]) > 6]
    return []


def scan(name, raw, private):
    if SECRET.search(raw) or any(value in raw for value in private):
        raise RuntimeError('Private data detected; upload blocked: ' + name)


def verify(target):
    manifest = json.loads((target / 'backup-manifest.json').read_text(encoding='utf-8'))
    private = known_private_values()
    for item in manifest['files']:
        p = target / item['path']
        raw = p.read_bytes()
        assert len(raw) == item['bytes'] and digest(raw) == item['sha256'], item['path']
        scan(item['path'], raw, private)
        if p.suffix == '.json':
            json.loads(raw)
        if p.suffix == '.py':
            compile(raw, str(p), 'exec')
    actual = {p.relative_to(target).as_posix() for p in target.rglob('*')
              if p.is_file() and '.git' not in p.relative_to(target).parts
              and '__pycache__' not in p.parts and p.name != 'backup-manifest.json'}
    assert actual == {item['path'] for item in manifest['files']}, 'Manifest coverage mismatch'
    print(json.dumps({'verified_files': len(actual), 'bytes': sum(i['bytes'] for i in manifest['files']),
                      'manifest_sha256': digest((target / 'backup-manifest.json').read_bytes())}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', type=Path, required=True, help='Existing public backup Git checkout')
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    target = args.target.resolve()
    assert source != target and (target / '.git').is_dir()
    assert git(target, 'remote', 'get-url', 'origin').rstrip('/') in (
        'https://github.com/dshanpi/Avaota4b-t736.git', 'https://github.com/dshanpi/Avaota4b-t736')
    if args.verify_only:
        verify(target)
        return
    tracked = set(git(target, 'ls-files').splitlines())
    names = {'README.md', 'status.json'}
    omitted = []
    for directory in SOURCE_DIRS:
        for p in (source / directory).rglob('*'):
            if not p.is_file():
                continue
            name = p.relative_to(source).as_posix()
            if '__pycache__' in p.parts or 'build' in p.relative_to(source).parts or p.suffix not in TEXT_SUFFIXES:
                omitted.append({'path': name, 'reason': 'generated/cache/non-source artifact'})
            else:
                names.add(name)
    referenced = set()
    for name in names:
        text = (source / name).read_text(encoding='utf-8-sig')
        referenced.update(re.findall(r'evidence/[A-Za-z0-9_./-]+\.(?:json|log|md|txt)', text))
    for p in (source / 'evidence').glob('*'):
        if not p.is_file():
            continue
        name = p.relative_to(source).as_posix()
        include = name in tracked or name in referenced or p.name.startswith(EVIDENCE_PREFIXES)
        if p.suffix in EVIDENCE_SUFFIXES and include and not p.name.startswith(OMIT_EVIDENCE):
            names.add(name)
    # Keep previously published records and files; missing local originals are not deleted.
    names.update(name for name in tracked if (source / name).is_file()
                 and name != 'backup-manifest.json' and (source / name).suffix in TEXT_SUFFIXES | EVIDENCE_SUFFIXES)
    names.update(name for name in referenced if (source / name).is_file()
                 and not Path(name).name.startswith(OMIT_EVIDENCE))
    names = {name for name in names if not name.startswith(OMIT_PATH_PREFIXES)
             and 'lynx_workbench_list_tasks' not in name}
    names.add('output/t736-armbian-source-v6.raw.manifest.json')
    names.add('evidence/source-v6-hdmi-framebuffer-20261114.png')
    private = known_private_values()
    copied = {}
    for name in sorted(names):
        p = source / name
        raw = p.read_bytes()
        original_sha = digest(raw)
        if p.suffix != '.png':
            text = raw.decode('utf-8-sig').replace('\r\n', '\n').replace('\r', '\n')
            if name == 'status.json':
                data = json.loads(text)
                data.pop('ssh', None)
                text = json.dumps(data, ensure_ascii=False, indent=2) + '\n'
            if p.suffix == '.log' or (p.suffix == '.txt' and name.startswith('evidence/')):
                text = ANSI.sub('', text).replace('\x08', '')
                text = re.sub(r'\n{3,}', '\n\n', text)
            raw = text.encode()
        scan(name, raw, private)
        out = target / name
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(raw)
        copied[name] = original_sha
    # Preserve original build hashes; record normalized public copies separately.
    ledger = target / 'config/patch-ledger.json'
    data = json.loads(ledger.read_text(encoding='utf-8'))
    for item in data['patches']:
        paths = item.get('source', [])
        paths = [paths] if isinstance(paths, str) else paths
        item['backup_file_sha256'] = {name: digest((target / name).read_bytes())
                                     for name in paths if (target / name).is_file()}
    write_json(ledger, data)
    existing_modes = {line.split('\t', 1)[1]: line.split(' ', 1)[0]
                      for line in git(target, 'ls-files', '--stage').splitlines()}
    files = []
    for p in sorted(target.rglob('*')):
        name = p.relative_to(target).as_posix()
        if not p.is_file() or '.git' in p.relative_to(target).parts or '__pycache__' in p.parts or name == 'backup-manifest.json':
            continue
        raw = p.read_bytes()
        scan(name, raw, private)
        files.append({'path': name, 'bytes': len(raw), 'sha256': digest(raw),
                      'git_blob_sha': hashlib.sha1(b'blob ' + str(len(raw)).encode() + b'\0' + raw).hexdigest(),
                      'mode': existing_modes.get(name, '100644'), 'workspace_sha256': copied.get(name)})
    manifest = {
        'date': '2026-11-15', 'timezone': 'Asia/Shanghai',
        'prepared_at': datetime.now(timezone(timedelta(hours=8))).isoformat(),
        'repository': 'https://github.com/dshanpi/Avaota4b-t736',
        'base_commit': git(target, 'rev-parse', 'HEAD'),
        'purpose': 'source-v6: fixed LPDDR4x, independent boot chain with authorized DDR library, HDMI console and board acceptance',
        'manifest_excludes_itself': True,
        'log_processing': 'UTF-8/LF; remove ANSI/backspace and excessive blank lines; retain failures; raw private originals remain local',
        'excluded_classes': ['credentials and MCP sessions', 'vendor binaries and embedded training firmware',
                             'compiled artifacts/raw images/rootfs/upstream checkout', 'vendor PDFs/screenshots/full extracted manuals',
                             'broad control-service inventories and unrelated raw board captures'],
        'binary_exception': 'Only the observed source-v6 HDMI framebuffer PNG; firmware hashes/manifests are text',
        'local_artifact_references': 'Evidence may reference local build/output/GPT binary files omitted by policy; register readback bytes remain in text logs',
        'omitted_source_artifacts': omitted,
        'files': files}
    write_json(target / 'backup-manifest.json', manifest)
    verify(target)


if __name__ == '__main__':
    main()
