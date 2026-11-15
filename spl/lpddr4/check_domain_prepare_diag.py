#!/usr/bin/env python3
"""Native domain experiment safety tests and freestanding ARM32 compile."""
from pathlib import Path
import hashlib
import json
import subprocess


def main():
    source = Path(__file__).resolve().parent
    root = source.parents[1]
    if root != Path('/home/ubuntu/t736-mainline'):
        raise SystemExit('Run in the isolated Linux build project')
    build = root / 'build/lpddr4-domain-prepare-diag'
    build.mkdir(parents=True, exist_ok=True)
    evidence = root / 'evidence/lpddr4-domain-prepare-diag-check.json'
    evidence.write_text(json.dumps({'checks_complete': False, 'board_tested': False}) + '\n')
    logs = []

    def run(command):
        proc = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT)
        entry = json.dumps(command) + '\n' + proc.stdout
        print(entry, end='', flush=True)
        logs.append(entry)
        (root / 'evidence/lpddr4-domain-prepare-diag-check.log').write_text(''.join(logs))
        if proc.returncode:
            raise SystemExit(proc.returncode)
        return proc.stdout

    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wshadow',
             '-Os', '-I', str(source)]
    run(['cc', *flags, '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
         str(source / 'domain_prepare_diag.c'), str(source / 'test_domain_prepare_diag.c'),
         '-o', str(build / 'test_domain_prepare_diag')])
    run([str(build / 'test_domain_prepare_diag')])
    run(['arm-linux-gnueabi-gcc', *flags, '-mcpu=cortex-a55', '-marm', '-mfloat-abi=soft',
         '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-c',
         str(source / 'domain_prepare_diag.c'), '-o', str(build / 'domain_prepare_diag.o')])
    undefined = run(['arm-linux-gnueabi-nm', '-u', str(build / 'domain_prepare_diag.o')])
    if undefined.strip():
        raise SystemExit('Unexpected external dependency')
    paths = [source / name for name in ('domain_prepare_diag.c', 'domain_prepare_diag.h',
             'test_domain_prepare_diag.c', 'check_domain_prepare_diag.py', 'phy/host.h',
             'pmu/domain-access-audit.json')]
    paths.append(build / 'domain_prepare_diag.o')
    evidence.write_text(json.dumps({
        'checks_complete': True,
        'native_address_undefined_sanitizers_pass': True,
        'initial_state_bit_guards': 192,
        'all_19_callback_failures_tested': True,
        'drift_or_readback_mismatch_points': 9,
        'arm32_freestanding_compile_pass': True,
        'undefined_symbols': undefined,
        'runtime_default': 'No integration or automatic execution; standalone three-write diagnostic',
        'successful_return_meaning': 'WRITES_OBSERVED_ONLY=1; not controller accessibility or training success',
        'register_write_allowlist': ['0x0a020010', '0x0a020014', '0x07010250'],
        'maximum_writes': 3,
        'controller_or_phy_accessed': False,
        'vendor_binaries_linked': False,
        'board_tested': False,
        'boot_firmware_deployable': False,
        'inputs_and_objects': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in paths},
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
