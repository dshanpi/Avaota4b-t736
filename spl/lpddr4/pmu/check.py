#!/usr/bin/env python3
"""Verify LP4 PMU components; never emit an executable firmware or deploy."""
from pathlib import Path
import hashlib
import json
import subprocess


def main():
    source = Path(__file__).resolve().parent
    root = source.parents[2]
    if root != Path('/home/ubuntu/t736-mainline'):
        raise SystemExit('Run in the isolated Linux build project')
    build = root / 'build/lpddr4-pmu-components'
    build.mkdir(parents=True, exist_ok=True)
    report_file = root / 'evidence/lpddr4-pmu-components-check.json'
    logs = []
    commands = []

    def run(command):
        commands.append(command)
        print(json.dumps(command), flush=True)
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        print(result.stdout, end='', flush=True)
        logs.append(json.dumps(command) + '\n' + result.stdout)
        if result.returncode:
            (root / 'evidence/lpddr4-pmu-components-check.log').write_text(''.join(logs))
            raise SystemExit(result.returncode)
        return result.stdout

    sources = [source / f'{name}.c' for name in ('common_message', 'mailbox', 'eye', 'commands')]
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wshadow',
             '-Os', '-I', str(source)]
    report_file.write_text(json.dumps({'checks_complete': False, 'deployable': False}) + '\n')
    run(['cc', *flags, '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
         *map(str, sources), str(source / 'test_pmu.c'), '-o', str(build / 'test_pmu')])
    run([str(build / 'test_pmu')])
    versions = {}
    for compiler, name, target_flags in (
        ('arm-linux-gnueabi-gcc', 'arm', ['-mcpu=cortex-a55', '-marm', '-mfloat-abi=soft']),
        ('arc-linux-gnu-gcc', 'arc', ['-mcpu=em4', '-mno-sdata', '-mmpy-option=mpy']),
    ):
        versions[compiler] = run([compiler, '--version']).splitlines()[0]
        for src in sources:
            run([compiler, *flags, *target_flags, '-ffreestanding', '-fno-builtin',
                 '-fno-stack-protector', '-c', str(src),
                 '-o', str(build / f'{name}-{src.stem}.o')])
    undefined = {}
    for prefix, name in (('arm-linux-gnueabi-', 'arm'), ('arc-linux-gnu-', 'arc')):
        objects = sorted(build.glob(f'{name}-*.o'))
        output = run([prefix + 'nm', '-u', *map(str, objects)])
        undefined[name] = output
        if ' U ' in output:
            raise SystemExit('Unexpected freestanding dependency')
    files = sorted([p for p in source.rglob('*') if p.is_file()] + list(build.glob('*.o')))
    evidence = {
        'checks_complete': True,
        'commands': commands,
        'compiler_versions': versions,
        'inputs_and_objects': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in files},
        'native_address_undefined_sanitizers_pass': True,
        'eye_exhaustive_oracle_cases': 40960,
        'mailbox_checked': ['32-bit stream payloads', 'payload containing success/fatal',
                            'success/fatal with stale nonzero high halfword',
                            'ready timeout', 'ack timeout', 'global poll budget',
                            'oversized stream', 'major message bound', 'I/O failures'],
        'common_offsets_checked_against_public_header': True,
        'arm32_compile_pass': True,
        'arc_em4_compile_pass': True,
        'undefined_symbols': undefined,
        'vendor_binaries_linked': False,
        'lpddr4_training_backend_implemented': False,
        'pmu_executable_built': False,
        'board_tested': False,
        'flashed': False,
        'coldboot_tested': False,
        'deployable': False,
    }
    report_file.write_text(json.dumps(evidence, indent=2) + '\n')
    (root / 'evidence/lpddr4-pmu-components-check.log').write_text(''.join(logs))


if __name__ == '__main__':
    main()
